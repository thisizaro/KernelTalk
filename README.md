# KernelTalk - Linux Kernel-based Chat System

A terminal-based chat system implemented as a custom Linux kernel module that enables user-space communication via character devices. This project demonstrates low-level IPC (Inter-Process Communication) and device driver programming using a producer-consumer model with blocking/non-blocking I/O operations.

> ⚙️ **Note**: This is a hobby project created for fun and learning.

---

## Features

- **Kernel Module**: Custom character device driver enabling chat functionality
- **User-Space Client**: Terminal-based chat client for communication
- **Producer-Consumer Model**: Efficient data handling with circular buffer
- **Blocking/Non-blocking I/O**: Supports both synchronous and asynchronous operations
- **Multi-client Support**: Multiple clients can connect simultaneously
- **Memory Management**: Proper kernel memory allocation and cleanup

---

## Architecture

The system consists of two main components:

1. **`kerneltalk_mod.c`** — Linux kernel module implementing:

   - Character device driver (`/dev/kerneltalk`)
   - Circular buffer for message storage
   - Client/server management per inode
   - Synchronization using mutexes and wait queues
   - Support for `select()` and `poll()` system calls

2. **`kerneltalk_client.c`** — User-space client providing:

   - Terminal interface for chat communication
   - File descriptor multiplexing using `select()`
   - Real-time message display and input

---

## Technical Details

### Kernel Module Features

- **Buffer Size**: 2048 bytes circular buffer
- **Synchronization**: Read-write semaphores and mutexes
- **Memory Management**: Dynamic allocation with proper cleanup
- **Device Operations**: `open`, `close`, `read`, `write`, `poll`
- **Multi-instance Support**: Per-inode server instances

### IPC Mechanism

- **Character Device**: `/dev/kerneltalk` serves as communication endpoint
- **Circular Buffer**: Efficient message storage and retrieval
- **Wait Queues**: Blocking I/O implementation
- **Select/Poll Support**: Integration with standard I/O multiplexing

---

## Building and Installation

### Prerequisites

- Linux kernel headers for your current kernel version
- GCC compiler
- Root privileges for module installation

### Build Process

```bash
# Clone the repository
git clone https://github.com/thisizaro/KernelTalk.git
cd KernelTalk

# Build both kernel module and client
make all

# Install the kernel module (requires root)
sudo make install
```

### Manual Installation

```bash
# Build kernel module
make module

# Build user-space client
make client

# Load the kernel module
sudo insmod kerneltalk_mod.ko

# Create device node
sudo mknod /dev/kerneltalk c $(grep kerneltalk /proc/devices | cut -d' ' -f1) 0
sudo chmod 666 /dev/kerneltalk
```

---

## Usage

### Starting a Chat Session

```bash
# Terminal 1 - Start first client
./kerneltalk_client /dev/kerneltalk

# Terminal 2 - Start second client
./kerneltalk_client /dev/kerneltalk
```

Now you can chat between the two terminals!

### Removing the Module

```bash
# Remove using Makefile
git clean -fdx
sudo make uninstall

# Or remove manually
sudo rmmod kerneltalk_mod
sudo rm -f /dev/kerneltalk
```

---

## Code Structure

### Kernel Module (`kerneltalk_mod.c`)

- **Data Structures**:

  - `kerneltalk_server`: Per-inode server management
  - `kerneltalk_client`: Per-process client tracking

- **Key Functions**:

  - `kerneltalk_open()` – Device open handler
  - `kerneltalk_read()` – Message reading with blocking support
  - `kerneltalk_write()` – Message writing with flow control
  - `kerneltalk_poll()` – Select/poll support

- **Synchronization**: Mutexes, read-write semaphores, and wait queues

### User Client (`kerneltalk_client.c`)

- **I/O Multiplexing**: Uses `select()` for real-time interaction
- **Buffer Management**: Handles message buffering and display
- **Error Handling**: Comprehensive error checking and reporting

---

## Technical Highlights

1. **Producer-Consumer Pattern**: Classic synchronization model
2. **Circular Buffer**: Efficient memory usage with wraparound logic
3. **Wait Queues**: Proper blocking I/O implementation
4. **File Operations**: Implements full character device interface
5. **Memory Safety**: Careful allocation/deallocation practices
6. **Multi-client Architecture**: Supports concurrent users seamlessly

---

## Development Notes

- Developed on Linux kernel **4.x and later**
- Follows Linux kernel coding standards
- Implements robust error handling and resource management
- Uses standard kernel APIs for character device driver development

---

## Tools Used

- **Language**: C
- **Environment**: Linux Kernel
- **Concepts**: IPC, Character Devices, Producer-Consumer Synchronization

---

## License

This project is licensed under the **GNU General Public License (GPL)** — see the [LICENSE](./LICENSE) file for details.

---

## Author

**Aranya Dutta**
[thisizaro.official@gmail.com](mailto:thisizaro.official@gmail.com)

---

## Contributing

Contributions are welcome! Please read the contributing guidelines (or open an issue) before submitting pull requests.

---
