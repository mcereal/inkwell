#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A pipe and a socket, born non-blocking and close-on-exec.
 *
 * Linux can say both at creation - pipe2(), SOCK_NONBLOCK | SOCK_CLOEXEC - and so these are one
 * call there. macOS cannot: it has neither, so the flags are set with fcntl() straight after, and
 * there is a window in which a fork() would inherit the descriptor. That window matters only to a
 * program that forks from a second thread, and nothing on this loop has a second thread.
 *
 * Everything on the loop wants both flags - a blocking descriptor stalls every other source, and
 * one that survives the resolver's fork is a descriptor the child holds open behind the parent's
 * back - which is why neither is a parameter.
 *
 * A socket from here does not raise SIGPIPE when its peer goes away, on either system: send it
 * with MSG_NOSIGNAL, which both have, and SO_NOSIGPIPE is set as well where it exists so a plain
 * write() is covered too.
 *
 * Each returns what it made - a descriptor, or 0 once `fds` is filled - or a negative errno, and
 * leaks nothing when it fails.
 */
int inkwell_fd_pipe(int fds[2]);
int inkwell_fd_socket(int domain, int type, int protocol);

/* Sets O_NONBLOCK and FD_CLOEXEC on a descriptor somebody else made. 0 or a negative errno. */
int inkwell_fd_set_nonblocking_cloexec(int fd);

/* A socket is pointer-sized on Windows, so it cannot safely travel through the int descriptor
 * API above. This value is a native socket on either host, never a loop registration token.
 * open() starts Winsock once on Windows and makes the socket nonblocking and non-inheritable.
 * The caller owns a successful result and closes it with inkwell_socket_close().
 * I/O returns a byte count or negative errno; EAGAIN means wait for loop readiness.
 */
typedef uintptr_t inkwell_socket;
#define INKWELL_SOCKET_INVALID UINTPTR_MAX

int inkwell_socket_open(int domain, int type, int protocol, inkwell_socket *out);
int inkwell_socket_close(inkwell_socket socket);
int inkwell_socket_connect(inkwell_socket socket, const void *address, size_t address_len);
int inkwell_socket_send(inkwell_socket socket, const void *bytes, size_t len);
int inkwell_socket_recv(inkwell_socket socket, void *bytes, size_t len);
/* 0 when connected and healthy, or a negative errno from SO_ERROR. */
int inkwell_socket_pending_error(inkwell_socket socket);

#ifdef __cplusplus
}
#endif
