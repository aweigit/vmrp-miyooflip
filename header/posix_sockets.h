#ifndef __POSIX_SOCKETS_H__
#define __POSIX_SOCKETS_H__

#include <stdio.h>

#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>

#define SOCKET_T int
#define SHUTDOWN_READ SHUT_RD
#define SHUTDOWN_WRITE SHUT_WR
#define SHUTDOWN_BIDIRECTIONAL SHUT_RDWR
#define SETSOCKOPT_PTR_TYPE const int *
#define SEND_RET_TYPE ssize_t
#define SEND_FORMATTING_SPECIFIER "%ld"
#define CLOSE_SOCKET(x) close(x)

#define GET_SOCKET_ERROR() (errno)

#define PRINT_SOCKET_ERROR(errorCode)                                           \
    do {                                                                        \
        printf("Call failed! errno: %s(%d)\n", strerror(errorCode), errorCode); \
    } while (0)

#endif
