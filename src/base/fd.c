/* _GNU_SOURCE for pipe2(), which glibc and musl both keep behind it. */
#define _GNU_SOURCE

#include "inkwell/base/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

int inkwell_fd_set_nonblocking_cloexec(int fd) {
    const int status = fcntl(fd, F_GETFL);
    if (status < 0 || fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0) {
        return -errno;
    }
    const int descriptor = fcntl(fd, F_GETFD);
    if (descriptor < 0 || fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) < 0) {
        return -errno;
    }
    return 0;
}

int inkwell_fd_pipe(int fds[2]) {
    if (fds == NULL) {
        return -EINVAL;
    }
#if defined(__linux__)
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
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
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
