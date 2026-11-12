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

/*
 * Who is at the other end of a connected Unix-domain socket, as the kernel recorded it at
 * connect() time. Each system spells the question differently - SO_PEERCRED on Linux,
 * getpeereid() and LOCAL_PEERPID on macOS - which is why it lives here rather than with the
 * socket's user.
 *
 * Returns 0 with uid and gid filled, or a negative errno; a field the system cannot say is -1, as
 * the pid can be on macOS. -ENOSYS where there is no way to ask.
 */
struct inkwell_fd_peer {
    int64_t pid;
    int64_t uid;
    int64_t gid;
};
int inkwell_fd_peer(int fd, struct inkwell_fd_peer *out);

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
/* Duplicate a CRT/POSIX descriptor so two owners have separate lifetimes - a test fixture and its
 * consumer, a queued send and its caller. Close-on-exec, as everything here is born; the file's
 * status flags (O_NONBLOCK) are the original's, shared. */
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

/*
 * Descriptors across a Unix-domain socket, riding with bytes (SCM_RIGHTS).
 *
 * The receiver gets its own descriptors for the same open files - a memory object, a pipe, a
 * device - which is how one process hands another something it could not open itself, or
 * memory both can map, without a path anyone else can reach. The descriptors arrive attached to
 * the first byte of the send that carried them, so a protocol on top knows which message they
 * belong to: whichever one that byte starts.
 *
 * send_fds() is send() with up to INKWELL_FD_PASS_MAX descriptors attached; the caller keeps its
 * own, which stay open. `len` must be at least 1 - descriptors need a byte to ride on.
 *
 * recv_fds() is recv() that also takes what arrived with the bytes, close-on-exec, into
 * `received`. Descriptors beyond INKWELL_FD_PASS_MAX are not kept: the kernel closes them, and
 * `received->lost` says so, which a protocol treats as a peer it can no longer trust to be in
 * step. Without recv_fds() a passed descriptor is simply closed on arrival.
 *
 * Both return a byte count or a negative errno. -ENOSYS on Windows.
 */
#define INKWELL_FD_PASS_MAX 4U
struct inkwell_fd_received {
    int fds[INKWELL_FD_PASS_MAX];
    size_t count;
    bool lost;
};
int inkwell_socket_send_fds(inkwell_socket socket, const void *bytes, size_t len, const int *fds,
                            size_t count);
int inkwell_socket_recv_fds(inkwell_socket socket, void *bytes, size_t len,
                            struct inkwell_fd_received *received);

/*
 * Memory two processes can map: an anonymous object with a descriptor and no name.
 *
 * The descriptor is the only way in - passed over a socket (above), it is how a producer hands a
 * consumer a buffer to read without a copy through the kernel. On Linux it is a memfd, sealed so
 * that it can never shrink: a reader whose mapping is cut short under it would fault on the next
 * read, so a producer must not be able to do that to it. macOS has no memfd; there it is a POSIX
 * shared-memory object unlinked at birth, whose size is fixed once set, which gives the same
 * guarantee. -ENOSYS on Windows.
 *
 * shared_memory() makes one of `size` bytes, zeroed, close-on-exec: a descriptor or a negative
 * errno.
 *
 * shared_memory_check() is what the receiving end asks of a descriptor it was handed before it
 * maps it: that it is a memory object at least `size` bytes long that cannot shrink. (macOS
 * rounds an object up to a whole page, so there "at least" is in pages.) 0, -EINVAL
 * when it is smaller or not memory, -EPERM when it could still be shrunk under the reader.
 *
 * map() maps `size` bytes from offset 0, shared, readable and writable when asked; unmap()
 * undoes it. 0 or a negative errno.
 */
int inkwell_fd_shared_memory(size_t size);
int inkwell_fd_shared_memory_check(int fd, size_t size);
int inkwell_fd_map(int fd, size_t size, bool writable, void **out);
int inkwell_fd_unmap(void *address, size_t size);

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
