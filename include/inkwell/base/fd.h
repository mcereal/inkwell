#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
/* Winsock must precede any windows.h include in a caller. */
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <sys/socket.h>
#endif

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

/* CRT/POSIX descriptor I/O, distinct from native socket I/O on Windows. A successful read or
 * write returns a byte count; a failure returns negative errno. close returns 0 or negative
 * errno. These do not make a descriptor nonblocking. */
int inkwell_fd_read(int fd, void *bytes, size_t len);
int inkwell_fd_write(int fd, const void *bytes, size_t len);
int inkwell_fd_close(int fd);
/* Creates `path`, or empties it, for writing: binary on Windows, where the CRT would otherwise
 * translate line endings, and not inherited by a child on either host. A descriptor for the
 * calls above, or a negative errno. */
int inkwell_fd_create(const char *path);
/* Duplicate a CRT/POSIX descriptor so a test fixture and its consumer own separate lifetimes. */
int inkwell_fd_dup(int fd);

/*
 * A device Windows has no non-blocking descriptor for - a COM port is a HANDLE read with
 * overlapped I/O - answering to an int all the same, so a stream and a loop can hold it as they
 * hold anything else. `fd` is the loop's token for the handle it waits on; once attached, the
 * three calls above on that number go to `ops` instead of the CRT, with `context` as given.
 *
 * read and write follow the calls they stand in for: a byte count, -EAGAIN until the handle is
 * signalled again, 0 from read when the device has gone, or a negative errno. close is the last
 * call the device hears: inkwell_fd_close() detaches it first. dup is refused with -ENOTSUP.
 * Returns 0, -EINVAL, -EEXIST for a number already attached, or -ENOSPC.
 *
 * Declared everywhere and refused with -ENOTSUP off Windows, where a device already has a
 * descriptor of its own and a number shadowed by callbacks would be a second meaning for it.
 */
struct inkwell_fd_device_ops {
    int (*read)(void *context, void *bytes, size_t len);
    int (*write)(void *context, const void *bytes, size_t len);
    int (*close)(void *context);
};

int inkwell_fd_attach_device(int fd, const struct inkwell_fd_device_ops *ops, void *context);

/* A socket is pointer-sized on Windows, so it cannot safely travel through the int descriptor
 * API above. This value is a native socket on either host, never a loop registration token.
 * open() starts Winsock once on Windows and makes the socket nonblocking and non-inheritable.
 * The caller owns a successful result and closes it with inkwell_socket_close().
 * I/O returns a byte count or negative errno; EAGAIN means wait for loop readiness.
 */
typedef uintptr_t inkwell_socket;
#define INKWELL_SOCKET_INVALID UINTPTR_MAX

/* POSIX accepts a socket as an int descriptor; Windows does not. This compatibility bridge
 * returns -ENOTSUP on Windows so an int cannot accidentally truncate a native SOCKET. */
int inkwell_fd_to_socket(int fd, inkwell_socket *out);
/* The same bridge the other way, for an API that still takes an int descriptor. -ENOTSUP on
 * Windows, with `*out` left at -1, for the same reason. */
int inkwell_socket_to_fd(inkwell_socket socket, int *out);

int inkwell_socket_open(int domain, int type, int protocol, inkwell_socket *out);
int inkwell_socket_close(inkwell_socket socket);
int inkwell_socket_connect(inkwell_socket socket, const void *address, size_t address_len);
int inkwell_socket_send(inkwell_socket socket, const void *bytes, size_t len);
int inkwell_socket_recv(inkwell_socket socket, void *bytes, size_t len);
/* 0 when connected and healthy, or a negative errno from SO_ERROR. */
int inkwell_socket_pending_error(inkwell_socket socket);

/* Parse a numeric IPv4/IPv6 address and attach port without doing a blocking name lookup.
 * A hostname or malformed literal returns false. This is safe before socket_open(). */
bool inkwell_socket_parse_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                                  socklen_t *out_len);

/* TCP socket policy, with unsupported optional keepalive tunables reported as -ENOTSUP.
 * A caller may ignore that result when platform defaults are acceptable. */
enum inkwell_socket_option {
    INKWELL_SOCKET_NO_DELAY,
    INKWELL_SOCKET_KEEPALIVE,
    INKWELL_SOCKET_KEEPALIVE_IDLE_S,
    INKWELL_SOCKET_KEEPALIVE_INTERVAL_S,
    INKWELL_SOCKET_KEEPALIVE_COUNT,
};
int inkwell_socket_set_option(inkwell_socket socket, enum inkwell_socket_option option,
                              unsigned value);

#ifdef __cplusplus
}
#endif
