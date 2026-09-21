#include "inkwell/runtime/wake.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

int inkwell_wake_open(struct inkwell_wake *wake) {
    if (wake == NULL) {
        return -EINVAL;
    }
    wake->fd = -1;
    wake->write_fd = -1;

#if defined(__linux__)
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    wake->fd = fd;
    wake->write_fd = fd;
#else
    int fds[2];
    const int opened = inkwell_fd_pipe(fds);
    if (opened < 0) {
        return opened;
    }
    wake->fd = fds[0];
    wake->write_fd = fds[1];
#endif
    return 0;
}

int inkwell_wake_signal(const struct inkwell_wake *wake) {
    if (wake == NULL || wake->write_fd < 0) {
        return -EINVAL;
    }
#if defined(__linux__)
    const uint64_t one = 1;
    const ssize_t written = write(wake->write_fd, &one, sizeof one);
#else
    const uint8_t one = 1;
    const ssize_t written = write(wake->write_fd, &one, sizeof one);
#endif
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -errno;
    }
    return 0;
}

int inkwell_wake_drain(const struct inkwell_wake *wake) {
    if (wake == NULL || wake->fd < 0) {
        return -EINVAL;
    }
    /* Eight bytes is exactly one eventfd read, and on a pipe it is eight signals at a time -
       looped until the pipe says it is empty. */
    bool pending = false;
    for (;;) {
        uint64_t buffer = 0;
        const ssize_t got = read(wake->fd, &buffer, sizeof buffer);
        if (got > 0) {
            pending = true;
#if defined(__linux__)
            break;
#else
            continue;
#endif
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -errno;
        }
        break;
    }
    return pending ? 1 : 0;
}

void inkwell_wake_close(struct inkwell_wake *wake) {
    if (wake == NULL) {
        return;
    }
    if (wake->write_fd >= 0 && wake->write_fd != wake->fd) {
        (void)close(wake->write_fd);
    }
    if (wake->fd >= 0) {
        (void)close(wake->fd);
    }
    wake->fd = -1;
    wake->write_fd = -1;
}
