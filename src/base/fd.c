/* _GNU_SOURCE for pipe2(), which glibc and musl both keep behind it. */
#define _GNU_SOURCE

#include "inkwell/base/fd.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define INKWELL_FD_DEVICES 8

struct fd_device {
    int fd;
    const struct inkwell_fd_device_ops *ops;
    void *context;
};

/* Only the loop's thread reads or writes a device, as it is the only thread that reads or writes
   anything else here. An unused entry has no ops. */
static struct fd_device s_devices[INKWELL_FD_DEVICES];

static struct fd_device *fd_device_find(int fd) {
    for (size_t i = 0; i < INKWELL_FD_DEVICES; ++i) {
        if (s_devices[i].ops != NULL && s_devices[i].fd == fd) {
            return &s_devices[i];
        }
    }
    return NULL;
}

#endif

int inkwell_fd_attach_device(int fd, const struct inkwell_fd_device_ops *ops, void *context) {
    if (fd < 0 || ops == NULL || ops->read == NULL || ops->write == NULL || ops->close == NULL) {
        return -EINVAL;
    }
#if !defined(_WIN32)
    (void)context;
    return -ENOTSUP;
#else
    if (fd_device_find(fd) != NULL) {
        return -EEXIST;
    }
    for (size_t i = 0; i < INKWELL_FD_DEVICES; ++i) {
        if (s_devices[i].ops == NULL) {
            s_devices[i] = (struct fd_device){.fd = fd, .ops = ops, .context = context};
            return 0;
        }
    }
    return -ENOSPC;
#endif
}

int inkwell_fd_read(int fd, void *bytes, size_t len) {
    if (fd < 0 || (bytes == NULL && len != 0U)) {
        return -EINVAL;
    }
    const unsigned count = (unsigned)(len > INT_MAX ? INT_MAX : len);
#if defined(_WIN32)
    const struct fd_device *device = fd_device_find(fd);
    if (device != NULL) {
        return device->ops->read(device->context, bytes, count);
    }
    const int result = _read(fd, bytes, count);
#else
    const int result = (int)read(fd, bytes, count);
#endif
    return result >= 0 ? result : -errno;
}

int inkwell_fd_write(int fd, const void *bytes, size_t len) {
    if (fd < 0 || (bytes == NULL && len != 0U)) {
        return -EINVAL;
    }
    const unsigned count = (unsigned)(len > INT_MAX ? INT_MAX : len);
#if defined(_WIN32)
    const struct fd_device *device = fd_device_find(fd);
    if (device != NULL) {
        return device->ops->write(device->context, bytes, count);
    }
    const int result = _write(fd, bytes, count);
#else
    const int result = (int)write(fd, bytes, count);
#endif
    return result >= 0 ? result : -errno;
}

int inkwell_fd_close(int fd) {
    if (fd < 0) {
        return -EINVAL;
    }
#if defined(_WIN32)
    struct fd_device *device = fd_device_find(fd);
    if (device != NULL) {
        const struct fd_device detached = *device;
        *device = (struct fd_device){0};
        return detached.ops->close(detached.context);
    }
    return _close(fd) == 0 ? 0 : -errno;
#else
    return close(fd) == 0 ? 0 : -errno;
#endif
}

int inkwell_fd_create(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
#if defined(_WIN32)
    const int fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
    return fd >= 0 ? fd : -errno;
}

int inkwell_fd_dup(int fd) {
    if (fd < 0) {
        return -EINVAL;
    }
#if defined(_WIN32)
    if (fd_device_find(fd) != NULL) {
        return -ENOTSUP;
    }
    const int duplicate = _dup(fd);
#else
    const int duplicate = dup(fd);
#endif
    return duplicate >= 0 ? duplicate : -errno;
}

int inkwell_fd_to_socket(int fd, inkwell_socket *out) {
    if (fd < 0 || out == NULL) {
        return -EINVAL;
    }
    *out = INKWELL_SOCKET_INVALID;
#if defined(_WIN32)
    return -ENOTSUP;
#else
    *out = (inkwell_socket)fd;
    return 0;
#endif
}

int inkwell_socket_to_fd(inkwell_socket socket, int *out) {
    if (socket == INKWELL_SOCKET_INVALID || out == NULL) {
        return -EINVAL;
    }
    *out = -1;
#if defined(_WIN32)
    return -ENOTSUP;
#else
    *out = (int)socket;
    return 0;
#endif
}

#if defined(_WIN32)
static INIT_ONCE kWinsockOnce = INIT_ONCE_STATIC_INIT;
static int s_winsock_error = EIO;

static int socket_error(int error) {
    switch (error) {
    case 0:
        return 0;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEALREADY:
        return EALREADY;
    case WSAENOPROTOOPT:
    case WSAEOPNOTSUPP:
        return ENOTSUP;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAEHOSTUNREACH:
        return EHOSTUNREACH;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAEACCES:
        return EACCES;
    case WSAEINVAL:
        return EINVAL;
    case WSAENOBUFS:
        return ENOBUFS;
    case WSAEAFNOSUPPORT:
        return EAFNOSUPPORT;
    case WSAENOTSOCK:
        return ENOTSOCK;
    case WSAEMSGSIZE:
        return EMSGSIZE;
    case WSAESHUTDOWN:
        return EPIPE;
    default:
        return EIO;
    }
}

static BOOL CALLBACK start_winsock(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    WSADATA data;
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        s_winsock_error = socket_error(result);
        return TRUE;
    }
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
        (void)WSACleanup();
        s_winsock_error = ENOSYS;
        return TRUE;
    }
    s_winsock_error = 0;
    return TRUE;
}
#endif

int inkwell_fd_set_nonblocking_cloexec(int fd) {
#if defined(_WIN32)
    (void)fd;
    return -ENOSYS;
#else
    const int status = fcntl(fd, F_GETFL);
    if (status < 0 || fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0) {
        return -errno;
    }
    const int descriptor = fcntl(fd, F_GETFD);
    if (descriptor < 0 || fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) < 0) {
        return -errno;
    }
    return 0;
#endif
}

int inkwell_fd_pipe(int fds[2]) {
    if (fds == NULL) {
        return -EINVAL;
    }
#if defined(_WIN32)
    fds[0] = -1;
    fds[1] = -1;
    return -ENOSYS;
#elif defined(__linux__)
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        return -errno;
    }
    return 0;
#else
    if (pipe(fds) != 0) {
        return -errno;
    }
    for (int i = 0; i < 2; ++i) {
        const int set = inkwell_fd_set_nonblocking_cloexec(fds[i]);
        if (set < 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            fds[0] = -1;
            fds[1] = -1;
            return set;
        }
    }
    return 0;
#endif
}

int inkwell_fd_socket(int domain, int type, int protocol) {
#if defined(_WIN32)
    (void)domain;
    (void)type;
    (void)protocol;
    return -ENOSYS;
#elif defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    const int fd = socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0) {
        return -errno;
    }
    return fd;
#else
    const int fd = socket(domain, type, protocol);
    if (fd < 0) {
        return -errno;
    }
    int result = inkwell_fd_set_nonblocking_cloexec(fd);
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    if (result == 0 && setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0) {
        result = -errno;
    }
#endif
    if (result < 0) {
        (void)close(fd);
        return result;
    }
    return fd;
#endif
}

int inkwell_socket_open(int domain, int type, int protocol, inkwell_socket *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    *out = INKWELL_SOCKET_INVALID;
#if defined(_WIN32)
    if (!InitOnceExecuteOnce(&kWinsockOnce, start_winsock, NULL, NULL)) {
        return -EIO;
    }
    if (s_winsock_error != 0) {
        return -s_winsock_error;
    }
    SOCKET socket = WSASocketW(domain, type, protocol, NULL, 0,
                               WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == INVALID_SOCKET) {
        return -socket_error(WSAGetLastError());
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, (long)FIONBIO, &nonblocking) != 0) {
        const int error = socket_error(WSAGetLastError());
        (void)closesocket(socket);
        return -error;
    }
    *out = (inkwell_socket)socket;
    return 0;
#else
    const int fd = inkwell_fd_socket(domain, type, protocol);
    if (fd < 0) {
        return fd;
    }
    *out = (inkwell_socket)fd;
    return 0;
#endif
}

int inkwell_socket_close(inkwell_socket socket) {
    if (socket == INKWELL_SOCKET_INVALID) {
        return -EINVAL;
    }
#if defined(_WIN32)
    return closesocket((SOCKET)socket) == 0 ? 0 : -socket_error(WSAGetLastError());
#else
    return close((int)socket) == 0 ? 0 : -errno;
#endif
}

int inkwell_socket_connect(inkwell_socket socket, const void *address, size_t address_len) {
    if (socket == INKWELL_SOCKET_INVALID || address == NULL || address_len > INT_MAX) {
        return -EINVAL;
    }
#if defined(_WIN32)
    if (connect((SOCKET)socket, (const struct sockaddr *)address, (int)address_len) == 0) {
        return 0;
    }
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? -EINPROGRESS : -socket_error(error);
#else
    return connect((int)socket, (const struct sockaddr *)address, (socklen_t)address_len) == 0
               ? 0
               : -errno;
#endif
}

int inkwell_socket_send(inkwell_socket socket, const void *bytes, size_t len) {
    if (socket == INKWELL_SOCKET_INVALID || (bytes == NULL && len != 0)) {
        return -EINVAL;
    }
    const int count = (int)(len > INT_MAX ? INT_MAX : len);
#if defined(_WIN32)
    const int sent = send((SOCKET)socket, (const char *)bytes, count, 0);
    return sent >= 0 ? sent : -socket_error(WSAGetLastError());
#else
    const int sent = (int)send((int)socket, bytes, (size_t)count, MSG_NOSIGNAL);
    return sent >= 0 ? sent : -errno;
#endif
}

int inkwell_socket_recv(inkwell_socket socket, void *bytes, size_t len) {
    if (socket == INKWELL_SOCKET_INVALID || (bytes == NULL && len != 0)) {
        return -EINVAL;
    }
    const int count = (int)(len > INT_MAX ? INT_MAX : len);
#if defined(_WIN32)
    const int received = recv((SOCKET)socket, (char *)bytes, count, 0);
    return received >= 0 ? received : -socket_error(WSAGetLastError());
#else
    const int received = (int)recv((int)socket, bytes, (size_t)count, 0);
    return received >= 0 ? received : -errno;
#endif
}

int inkwell_socket_pending_error(inkwell_socket socket) {
    if (socket == INKWELL_SOCKET_INVALID) {
        return -EINVAL;
    }
    int error = 0;
#if defined(_WIN32)
    int len = sizeof error;
    if (getsockopt((SOCKET)socket, SOL_SOCKET, SO_ERROR, (char *)&error, &len) != 0) {
        return -socket_error(WSAGetLastError());
    }
    return -socket_error(error);
#else
    socklen_t len = sizeof error;
    if (getsockopt((int)socket, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
        return -errno;
    }
    return -error;
#endif
}

bool inkwell_socket_parse_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                                  socklen_t *out_len) {
    if (host == NULL || host[0] == '\0' || out == NULL || out_len == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct in_addr v4;
#if defined(_WIN32)
    const int parsed_v4 = InetPtonA(AF_INET, host, &v4);
#else
    const int parsed_v4 = inet_pton(AF_INET, host, &v4);
#endif
    if (parsed_v4 == 1) {
        struct sockaddr_in *address = (struct sockaddr_in *)out;
        address->sin_family = AF_INET;
        address->sin_port = htons(port);
        address->sin_addr = v4;
        *out_len = (socklen_t)sizeof *address;
        return true;
    }

    struct in6_addr v6;
#if defined(_WIN32)
    const int parsed_v6 = InetPtonA(AF_INET6, host, &v6);
#else
    const int parsed_v6 = inet_pton(AF_INET6, host, &v6);
#endif
    if (parsed_v6 == 1) {
        struct sockaddr_in6 *address = (struct sockaddr_in6 *)out;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(port);
        address->sin6_addr = v6;
        *out_len = (socklen_t)sizeof *address;
        return true;
    }
    return false;
}

int inkwell_socket_set_option(inkwell_socket socket, enum inkwell_socket_option option,
                              unsigned value) {
    if (socket == INKWELL_SOCKET_INVALID || value > (unsigned)INT_MAX) {
        return -EINVAL;
    }
    int level = IPPROTO_TCP;
    int name = 0;
    switch (option) {
    case INKWELL_SOCKET_NO_DELAY:
        name = TCP_NODELAY;
        break;
    case INKWELL_SOCKET_KEEPALIVE:
        level = SOL_SOCKET;
        name = SO_KEEPALIVE;
        break;
    case INKWELL_SOCKET_KEEPALIVE_IDLE_S:
#if defined(TCP_KEEPIDLE)
        name = TCP_KEEPIDLE;
#elif defined(TCP_KEEPALIVE)
        name = TCP_KEEPALIVE;
#else
        return -ENOTSUP;
#endif
        break;
    case INKWELL_SOCKET_KEEPALIVE_INTERVAL_S:
#if defined(TCP_KEEPINTVL)
        name = TCP_KEEPINTVL;
#else
        return -ENOTSUP;
#endif
        break;
    case INKWELL_SOCKET_KEEPALIVE_COUNT:
#if defined(TCP_KEEPCNT)
        name = TCP_KEEPCNT;
#else
        return -ENOTSUP;
#endif
        break;
    default:
        return -EINVAL;
    }
    const int setting = (int)value;
#if defined(_WIN32)
    return setsockopt((SOCKET)socket, level, name, (const char *)&setting, sizeof setting) == 0
               ? 0
               : -socket_error(WSAGetLastError());
#else
    if (setsockopt((int)socket, level, name, &setting, sizeof setting) == 0) {
        return 0;
    }
    return errno == ENOPROTOOPT || errno == EOPNOTSUPP ? -ENOTSUP : -errno;
#endif
}
