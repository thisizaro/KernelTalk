/*
 * KernelTalk: kernel based chat
 *
 * This module implements a character device which allows chatting between
 * user-space programs via the read() and write() system calls.
 */

#include <linux/kernel.h>  /* it's the kernel yo */
#include <linux/module.h>  /* it's a module yo */
#include <linux/init.h>	   /* for module_{init,exit} */
#include <linux/slab.h>	   /* kmalloc */
#include <linux/fs.h>	   /* file_operations, file, etc... */
#include <linux/list.h>	   /* all the list stuff */
#include <linux/mutex.h>   /* struct mutex */
#include <linux/rwsem.h>   /* struct rw_semaphore */
#include <linux/poll.h>	   /* for the polling/select stuff! */
#include <linux/sched.h>   /* poll.h doesn't always include this */
#include <linux/wait.h>	   /* for wait queues */
#include <linux/uaccess.h> /* for put_user */

#define KERNELTALK_VMAJOR 0
#define KERNELTALK_VMINOR 1
#define SUCCESS 0
#define DEVICE_NAME "kerneltalk"
#define KERNELTALK_BUF 2048

#define DIST(a, b) ((a) <= (b) ? (b) - (a) : KERNELTALK_BUF + (b) - (a))

static int kerneltalk_open(struct inode *, struct file *);
static int kerneltalk_flush(struct file *, fl_owner_t);
static ssize_t kerneltalk_read(struct file *, char *, size_t, loff_t *);
static ssize_t kerneltalk_write(struct file *, const char *, size_t, loff_t *);
static unsigned int kerneltalk_poll(struct file *, poll_table *);

/*
 * Chat server exists per-inode.
 */
struct kerneltalk_server
{
	struct inode *inode;
	struct list_head server_list;  // CONTAINED IN this list
	struct list_head client_list;  // CONTAINS this list
	struct mutex client_list_lock; // protects client_list
	wait_queue_head_t rwq;		   // whom to wake when data is available
	wait_queue_head_t wwq;		   // whom to wake when room is available
	char buffer[KERNELTALK_BUF];
	struct rw_semaphore buffer_lock;
	int end;
};

/*
 * Chat client exists per-process, per-file. Has reference to server and its own
 * unique offset.
 */
struct kerneltalk_client
{
	struct file *filp;
	struct kerneltalk_server *server;
	struct list_head client_list; /* CONTAINED IN this list */
	int offset;
};

/*
 * This is the global server list. One per inode.
 */
static LIST_HEAD(server_list);
DEFINE_MUTEX(server_list_lock);

/*
 * Major number of our character device.
 */
static int major;

/*
 * Our file operations. These are registered for our character device to handle
 * open, close, read, write calls to our special device files.
 */
static struct file_operations kerneltalk_fops = {
	.read = kerneltalk_read,
	.write = kerneltalk_write,
	.open = kerneltalk_open,
	.flush = kerneltalk_flush,
	.poll = kerneltalk_poll,
	.owner = THIS_MODULE};

/*
 * Create a chat server for an inode. This assumes one does not already exist.
 * server_list_lock MUST already be held at this point
 * MUST check for null return (ENOMEM)
 */
static struct kerneltalk_server *create_server(struct inode *inode)
{
	struct kerneltalk_server *srv;

	srv = kmalloc(sizeof(struct kerneltalk_server), GFP_KERNEL);
	// Early return for NULL! Must be checked.
	if (srv == NULL)
	{
		return NULL;
	}

	srv->inode = inode;
	srv->end = 0;
	INIT_LIST_HEAD(&srv->server_list);
	INIT_LIST_HEAD(&srv->client_list);
	mutex_init(&srv->client_list_lock);
	init_rwsem(&srv->buffer_lock);
	init_waitqueue_head(&srv->rwq);
	init_waitqueue_head(&srv->wwq);
	list_add(&srv->server_list, &server_list);

	return srv;
}

/*
 * Get the chat server for an inode, creating it if it doesn't exist.
 * requires the server_list_lock to be held
 */
static struct kerneltalk_server *get_server(struct inode *inode)
{
	struct kerneltalk_server *srv;

	list_for_each_entry(srv, &server_list, server_list)
	{
		if (srv->inode == inode)
		{
			return srv;
		}
	}

	printk(KERN_INFO "kerneltalk: get_server: inode=%p creating new server\n",
		   inode);
	srv = create_server(inode);
	return srv;
}

/*
 * Free server if it has no clients.
 * server_list lock must be held, will also try to hold the client list lock
 */
static void check_free_server(struct kerneltalk_server *srv)
{
	// for safety, always lock server, then client when you need both
	mutex_lock_interruptible(&srv->client_list_lock);
	if (list_empty(&srv->client_list))
	{
		// remove us from the server list
		list_del(&srv->server_list);
		printk(KERN_INFO "kerneltalk: check_free_server: freeing srv->inode=%p\n", srv->inode);
		kfree(srv);
	}
	else
	{
		printk(KERN_INFO "kerneltalk: check_free_server: not freeing srv->inode=%p\n", srv->inode);
		// only release the client list lock if there are still clients
		mutex_unlock(&srv->client_list_lock);
	}
}

/*
 * Return the offset of the client with the most unread data.
 * client_list_lock must be held for this server, as well as write lock if you
 * want accurate numbers...
 */
static int blocking_offset(struct kerneltalk_server *srv)
{
	struct kerneltalk_client *cnt;
	int maxunread = 0;
	int offset = srv->end;

	list_for_each_entry(cnt, &srv->client_list, client_list)
	{
		if (DIST(cnt->offset, srv->end) > maxunread)
		{
			maxunread = DIST(cnt->offset, srv->end);
			offset = cnt->offset;
		}
	}

	return offset;
}

/*
 * Convenience function for determining how many bytes we have room to write in
 * our buffer. It grabs the client_list lock, finds the offset with the most
 * unread data, and goes ONE BEFORE it (because if end==offset, we expect that
 * there is no data to read). Then it computes the distance from the end of the
 * data to this offset: which is the amount of room we have for writing.
 */
static int room_to_write(struct kerneltalk_server *srv)
{
	int maxidx;

	mutex_lock_interruptible(&srv->client_list_lock);
	maxidx = (blocking_offset(srv) + KERNELTALK_BUF - 1) % KERNELTALK_BUF;
	mutex_unlock(&srv->client_list_lock);

	return DIST(srv->end, maxidx);
}

/*
 * Create a new client for a server. Client objects are stored within struct
 * file's private_data field, so there is no need for any special lookup from
 * the client list when you want this client again.
 */
static struct kerneltalk_client *create_client(struct file *filp,
											   struct kerneltalk_server *srv)
{
	struct kerneltalk_client *cnt;

	cnt = kmalloc(sizeof(struct kerneltalk_client), GFP_KERNEL);
	if (cnt == NULL)
		return NULL;

	cnt->filp = filp;
	cnt->server = srv;
	INIT_LIST_HEAD(&cnt->client_list);
	cnt->offset = srv->end; // prevent invalid data
	filp->private_data = cnt;

	mutex_lock_interruptible(&srv->client_list_lock);
	list_add(&cnt->client_list, &srv->client_list);
	mutex_unlock(&srv->client_list_lock);

	return cnt;
}

/*
 * FILE OPERATIONS
 */

/*
 * Open - will get/create a server, and create a client as well
 */
static int kerneltalk_open(struct inode *inode, struct file *filp)
{
	struct kerneltalk_server *srv;
	struct kerneltalk_client *cnt;

	// Obtain server list lock so that everything happens atomically to the
	// server list.
	mutex_lock_interruptible(&server_list_lock);

	srv = get_server(inode);
	if (!srv)
		goto server_create_failed;

	cnt = create_client(filp, srv);
	if (!cnt)
		goto client_create_failed;

	mutex_unlock(&server_list_lock);

	printk(KERN_INFO "kerneltalk: open: inode=%p filp=%p opened file!\n",
		   inode, filp);
	return SUCCESS;

client_create_failed:
	// free the server if we were the only ones who wanted it
	check_free_server(srv);
server_create_failed:
	mutex_unlock(&server_list_lock);
	printk(KERN_INFO "kerneltalk: open: inode=%p filp=%p fail\n", inode, filp);
	return -ENOMEM;
}

/*
 * Flush - called when a process closes their file descriptor, and so any
 * buffered data of theirs should be flushed. We use this as a notification when
 * close() is called so that we can free the client and optionally the server
 * when it has no more clients.
 *
 * NOTE Due to fork() or dup(), multiple processes and file descriptors can
 * refer to the same struct file!! If any of them are closed, this will be
 * called. Therefore, we check the f_count value to ensure that no more file
 * descriptors are using this file before we free it.
 *
 * NOTE This is just for safety. If two processes/threads have the same file
 * instance and they both read/write, bad things will happen and I'll yell at
 * them. I just would prefer to not make the kernel segfault when one of them
 * closes their copy.
 */
static int kerneltalk_flush(struct file *filp, fl_owner_t unused)
{
	struct kerneltalk_client *cnt;
	long count = atomic_long_read(&filp->f_count);

	if (count != 1)
	{
		printk(KERN_INFO "kerneltalk: flush: filp=%p filp->f_count=%ld bailing!\n", filp,
			   count);
		return SUCCESS;
	}

	cnt = filp->private_data;

	mutex_lock_interruptible(&cnt->server->client_list_lock);
	list_del(&cnt->client_list);
	mutex_unlock(&cnt->server->client_list_lock);

	mutex_lock_interruptible(&server_list_lock);
	check_free_server(cnt->server);
	mutex_unlock(&server_list_lock);

	kfree(cnt);

	printk(KERN_INFO "kerneltalk: flush: filp=%p freed client\n", filp);
	return SUCCESS;
}

/*
 * Read - read from the server. This has blocking and non-blocking variations.
 */
static ssize_t kerneltalk_read(struct file *filp, char *usrbuf, size_t length,
							   loff_t *offset)
{
	struct kerneltalk_server *srv;
	struct kerneltalk_client *cnt;
	int bytes_read = 0;

	printk(KERN_INFO "kerneltalk: read: filp=%p WAIT FOR DATA\n", filp);

	cnt = filp->private_data;
	srv = cnt->server;

	// acquire buffer read lock to ensure amount of data doesn't change
	down_read(&srv->buffer_lock);

	// wait till we have data
	while (DIST(cnt->offset, srv->end) == 0)
	{
		up_read(&srv->buffer_lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(srv->rwq, DIST(cnt->offset, srv->end) != 0))
			return -ERESTARTSYS;
		down_read(&srv->buffer_lock);
	}

	printk(KERN_INFO "kerneltalk: read: filp=%p READING length=%zu srv->end=%d cnt->offset=%d\n",
		   filp, length, srv->end, cnt->offset);

	while (length && DIST(cnt->offset, srv->end) > 0)
	{
		put_user(srv->buffer[cnt->offset], usrbuf++);
		length--;
		cnt->offset = (cnt->offset + 1) % KERNELTALK_BUF;
		bytes_read++;
	}

	up_read(&srv->buffer_lock);

	printk(KERN_INFO "kerneltalk: read: filp=%p READ %d, length=%zu srv->end=%d cnt->offset=%d\n",
		   filp, bytes_read, length, srv->end, cnt->offset);

	wake_up(&srv->wwq); // there may be more room now that we've read
	return bytes_read;
}

/*
 * Return information about whether the file is ready to read or write.
 * Additionally register our wait queues with the poll table so that the select
 * and poll system calls can wake when the state changes.
 */
static unsigned int kerneltalk_poll(struct file *filp, poll_table *tbl)
{
	struct kerneltalk_client *cnt;
	struct kerneltalk_server *srv;
	int mask = 0;

	cnt = filp->private_data;
	srv = cnt->server;

	printk(KERN_INFO "kerneltalk: poll filp=%p\n", filp);

	poll_wait(filp, &srv->rwq, tbl);
	poll_wait(filp, &srv->wwq, tbl);

	/*
	 * We need the write lock here so that we can be certain no readers are
	 * reading, and thus updating their offsets while we check them.
	 */
	down_write(&srv->buffer_lock);

	mask = 0;
	if (DIST(cnt->offset, srv->end) > 0)
	{
		mask |= POLLIN | POLLRDNORM;
	}

	if (room_to_write(srv) > 0)
	{
		mask |= POLLOUT | POLLWRNORM;
	}

	up_write(&srv->buffer_lock);
	return mask;
}

/*
 * Write - Put user data into the buffer. Supports blocking and non-blocking
 * variations. Requires mutual exclusion from all readers and writers for
 * safety.
 */
ssize_t kerneltalk_write(struct file *filp, const char *usrbuf, size_t amt,
						 loff_t *unused)
{
	struct kerneltalk_client *cnt;
	struct kerneltalk_server *srv;
	int room;
	int bytes_written = 0;

	cnt = filp->private_data;
	srv = cnt->server;

	printk(KERN_INFO "kerneltalk: write: filp=%p WAIT FOR ROOM\n", filp);

	down_write(&srv->buffer_lock);

	// wait until there is room to write
	while ((room = room_to_write(srv)) == 0)
	{
		up_write(&srv->buffer_lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(srv->wwq, room_to_write(srv) > 0))
			return -ERESTARTSYS;
		down_write(&srv->buffer_lock);
	}

	printk(KERN_INFO "kerneltalk: write: filp=%p WRITING room=%d amt=%zu srv->end=%d\n",
		   filp, room, amt, srv->end);

	while (room > 0 && amt > 0)
	{
		get_user(srv->buffer[srv->end], usrbuf++);
		srv->end = (srv->end + 1) % KERNELTALK_BUF;
		amt--;
		room--;
		bytes_written++;
	}

	up_write(&srv->buffer_lock);

	printk(KERN_INFO "kerneltalk: write: filp=%p WROTE %d, room=%d amt=%zu srv->end=%d\n",
		   filp, bytes_written, room, amt, srv->end);

	wake_up(&srv->rwq); // there is more data for readers
	return bytes_written;
}

/*
 * Module initialization and exit routines.
 */
static int __init init_kerneltalk(void)
{
	major = register_chrdev(0, DEVICE_NAME, &kerneltalk_fops);
	if (major < 0)
	{
		printk(KERN_ALERT "Registering char device failed with %d\n",
			   major);
		return major;
	}

	printk(KERN_INFO "kerneltalk v%d.%d -- assigned major number %d\n",
		   KERNELTALK_VMAJOR, KERNELTALK_VMINOR, major);
	printk(KERN_INFO "'mknod /dev/kerneltalk c %d 0' to make chat file!\n", major);

	return SUCCESS;
}

static void __exit exit_kerneltalk(void)
{
	unregister_chrdev(major, DEVICE_NAME);
	if (!list_empty(&server_list))
	{
		printk(KERN_ALERT "Uh-oh: kerneltalk module unloaded without "
						  "all files being closed!\n");
	}
}

module_init(init_kerneltalk);
module_exit(exit_kerneltalk);

MODULE_AUTHOR("Aranya Dutta <thisisaro.official@gmail.com>");
MODULE_DESCRIPTION("Driver for simple chat between processes via character devices.");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
