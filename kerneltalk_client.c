/*
 * KernelTalk: kernel based chat
 *
 * This is a user-space client for the KernelTalk kernel chat server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <string.h>
#include <errno.h>

#define KERNELTALK_BUF 2048

void writeall(int fildes, char *buf, int amt)
{
    ssize_t rv;
    while (amt > 0)
    {
        rv = write(fildes, buf, amt);
        if (rv < 0)
        {
            perror("kerneltalk");
            exit(EXIT_FAILURE);
        }
        buf += rv;
        amt -= rv;
    }
}

int main(int argc, char **argv)
{
    char *buf;
    char inbuf[KERNELTALK_BUF];
    size_t n = 0;
    ssize_t len;
    FILE *f;
    fd_set rfds;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s FILENAME\n", argv[0]);
        return EXIT_FAILURE;
    }

    f = fopen(argv[1], "r+");
    if (!f)
    {
        perror("kerneltalk");
        return EXIT_FAILURE;
    }

    FD_ZERO(&rfds);
    FD_SET(fileno(stdin), &rfds);
    FD_SET(fileno(f), &rfds);

    while (select(fileno(f) + 1, &rfds, NULL, NULL, NULL) != -1)
    {
        if (FD_ISSET(fileno(stdin), &rfds))
        {
            len = getline(&buf, &n, stdin);
            writeall(fileno(f), buf, len);
        }
        if (FD_ISSET(fileno(f), &rfds))
        {
            len = read(fileno(f), inbuf, KERNELTALK_BUF);
            if (len < 0)
            {
                perror("kerneltalk");
                exit(EXIT_FAILURE);
            }
            writeall(fileno(stdout), inbuf, len);
        }
        FD_SET(fileno(stdin), &rfds);
        FD_SET(fileno(f), &rfds);
    }

    return 0;
}
