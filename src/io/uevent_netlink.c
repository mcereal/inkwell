#include "inkwell/io/uevent.h"

#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The kernel's group; udevd's re-broadcast is 2 and is not joined. */
#define UEVENT_GROUP_KERNEL 1U
/* A kernel uevent is at most UEVENT_BUFFER_SIZE (2048) bytes; anything larger is not one. */
#define UEVENT_MESSAGE_MAX 8192
/* A burst - coldplug, a hub full of devices - is hundreds of events at once. */
#define UEVENT_RCVBUF (1 << 20)
/* Messages taken per wakeup. The loop is level-triggered, so whatever is left is taken on the
   next turn; draining a burst in one go would hold every other source off until it ended. */
#define UEVENT_READ_BUDGET 64

/* Runs the callback; nonzero when it closed the monitor, which may since have been freed or
   opened again - either way the caller must not touch it. */
static int deliver(struct inkwell_uevent_monitor *monitor, const struct inkwell_uevent *event) {
    int closed = 0;
    monitor->closed = &closed;
    monitor->callback(event, monitor->userdata);
    if (closed) {
        return 1;
    }
    monitor->closed = NULL;
    return 0;
}

static void receive(struct inkwell_uevent_monitor *monitor, int fd) {
    char buf[UEVENT_MESSAGE_MAX];
    for (int taken = 0; taken < UEVENT_READ_BUDGET; ++taken) {
        struct sockaddr_nl sender;
        struct iovec iov = {buf, sizeof buf};
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_name = &sender;
        msg.msg_namelen = sizeof sender;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        const ssize_t got = recvmsg(fd, &msg, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOBUFS) {
                /* The kernel dropped events while the buffer was full. */
                if (deliver(monitor, NULL)) {
                    return;
                }
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                inkwell_log_warn("uevent", "receive failed: %s", strerror(errno));
            }
            return;
        }
        /* Only the kernel: netlink port 0. Anything else on the group is not a device event. */
        if (msg.msg_namelen != sizeof sender || sender.nl_family != AF_NETLINK ||
            sender.nl_pid != 0 || (msg.msg_flags & MSG_TRUNC) != 0) {
            continue;
        }
        struct inkwell_uevent event;
        if (inkwell_uevent_parse(buf, (size_t)got, &event) != 0) {
            continue;
        }
        if (deliver(monitor, &event)) {
            return;
        }
    }
}

static int on_readable(int fd, uint32_t events, void *userdata) {
    (void)events;
    receive((struct inkwell_uevent_monitor *)userdata, fd);
    return 0;
}

int inkwell_uevent_monitor_open(struct inkwell_uevent_monitor *monitor, struct inkwell_loop *loop,
                                inkwell_uevent_callback callback, void *userdata) {
    if (monitor == NULL || loop == NULL || callback == NULL) {
        return -EINVAL;
    }
    if (monitor->loop != NULL) {
        return -EBUSY;
    }
    const int fd = inkwell_fd_socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        return fd;
    }
    /* FORCE goes past rmem_max and needs CAP_NET_ADMIN; without it, as much as is allowed. */
    const int size = UEVENT_RCVBUF;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof size) != 0) {
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    }
    struct sockaddr_nl address;
    memset(&address, 0, sizeof address);
    address.nl_family = AF_NETLINK;
    address.nl_groups = UEVENT_GROUP_KERNEL;
    if (bind(fd, (const struct sockaddr *)&address, sizeof address) != 0) {
        const int error = -errno;
        (void)close(fd);
        return error;
    }
    monitor->fd = fd;
    monitor->callback = callback;
    monitor->userdata = userdata;
    monitor->closed = NULL;
    const int added = inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, on_readable, monitor);
    if (added < 0) {
        (void)close(fd);
        monitor->fd = -1;
        monitor->callback = NULL;
        monitor->userdata = NULL;
        return added;
    }
    monitor->loop = loop;
    return 0;
}

void inkwell_uevent_monitor_close(struct inkwell_uevent_monitor *monitor) {
    if (monitor == NULL || monitor->loop == NULL) {
        return;
    }
    if (monitor->closed != NULL) {
        *monitor->closed = 1;
        monitor->closed = NULL;
    }
    (void)inkwell_loop_remove_fd(monitor->loop, monitor->fd);
    (void)close(monitor->fd);
    monitor->loop = NULL;
    monitor->fd = -1;
    monitor->callback = NULL;
    monitor->userdata = NULL;
}

int inkwell_uevent_trigger(const char *syspath, const char *action) {
    if (syspath == NULL || action == NULL || action[0] == '\0') {
        return -EINVAL;
    }
    char path[PATH_MAX];
    const int n = snprintf(path, sizeof path, "%s/uevent", syspath);
    if (n < 0 || (size_t)n >= sizeof path) {
        return -ENAMETOOLONG;
    }
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    const size_t len = strlen(action);
    ssize_t wrote;
    do {
        wrote = write(fd, action, len);
    } while (wrote < 0 && errno == EINTR);
    const int result = wrote < 0 ? -errno : (size_t)wrote == len ? 0 : -EIO;
    (void)close(fd);
    return result;
}
