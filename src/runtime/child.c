#include "inkwell/runtime/child.h"

#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
/* New system calls share one number on every architecture; older libc headers lack the names. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#else
#include <fcntl.h>
#include <sys/event.h>
#include <time.h>
#endif

_Static_assert(sizeof(pid_t) == sizeof(int), "a pid fits the int child.h keeps it in");

/* ---- the backend ----------------------------------------------------------------------------- */

#if defined(__linux__)

static int watch_open(pid_t pid) {
    const long fd = syscall(SYS_pidfd_open, pid, 0);
    return fd < 0 ? -errno : (int)fd;
}

/* A pidfd stays readable once the process has exited, until the descriptor is closed. */
static void watch_drain(int fd) {
    (void)fd;
}

static int watch_retry(int fd) {
    (void)fd;
    return 0;
}

static int watch_signal(const struct inkwell_child *child, int signal) {
    if (syscall(SYS_pidfd_send_signal, child->fd, signal, NULL, 0) == 0) {
        return 0;
    }
    if (errno != ENOSYS) {
        return -errno;
    }
    /* Every kernel with pidfd_open() (5.3) has this (5.1), so ENOSYS here is a filter - a seccomp
       profile - that let one through and not the other. An unreaped child still owns its pid,
       so kill() reaches the same process. */
    return kill(child->pid, signal) < 0 ? -errno : 0;
}

#else

/* The one other event the kqueue may hold, for when the exit has to be looked at again. */
#define RETRY_IDENT 1U

/*
 * Makes the kqueue readable again in a millisecond.
 *
 * Two cases need it. A child that had already exited when it was watched cannot carry a
 * NOTE_EXIT - Darwin refuses EVFILT_PROC on a zombie with ESRCH - so the watch reports it on
 * the next turn instead. And NOTE_EXIT is posted as the process exits, which can be a moment
 * before waitpid() can collect it; a WNOHANG that comes back empty looks again shortly rather
 * than losing the exit it was told about.
 */
static int watch_arm_retry(int kq) {
    struct kevent change;
    EV_SET(&change, RETRY_IDENT, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1, NULL);
    return kevent(kq, &change, 1, NULL, 0, NULL) < 0 ? -errno : 0;
}

static int watch_open(pid_t pid) {
    const int kq = kqueue();
    if (kq < 0) {
        return -errno;
    }
    (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
    struct kevent change;
    EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    int result = kevent(kq, &change, 1, NULL, 0, NULL) < 0 ? -errno : 0;
    if (result == -ESRCH) {
        result = watch_arm_retry(kq);
    }
    if (result < 0) {
        (void)close(kq);
        return result;
    }
    return kq;
}

static void watch_drain(int kq) {
    const struct timespec now = {0, 0};
    struct kevent events[2];
    while (kevent(kq, NULL, 0, events, 2, &now) < 0 && errno == EINTR) {
    }
}

static int watch_retry(int kq) {
    const int armed = watch_arm_retry(kq);
    if (armed < 0) {
        inkwell_log_warn("child", "re-arming an exit check failed: %s", strerror(-armed));
    }
    return armed;
}

static int watch_signal(const struct inkwell_child *child, int signal) {
    return kill(child->pid, signal) < 0 ? -errno : 0;
}

#endif

/* ---- the watch ------------------------------------------------------------------------------- */

static void release(struct inkwell_child *child) {
    if (child->loop != NULL) {
        (void)inkwell_loop_remove_fd(child->loop, child->fd);
    }
    if (child->fd >= 0) {
        (void)close(child->fd);
    }
    /* `pid` is kept: it is what the exit callback reads to know which child this was. */
    child->loop = NULL;
    child->fd = -1;
    child->exited = NULL;
    child->userdata = NULL;
}

static int on_ready(int fd, uint32_t events, void *userdata) {
    (void)events;
    struct inkwell_child *child = (struct inkwell_child *)userdata;
    watch_drain(fd);

    int status = 0;
    pid_t got;
    do {
        got = waitpid((pid_t)child->pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == 0) {
        if (watch_retry(fd) == 0) {
            return 0;
        }
        /* Nothing would wake this watch again. The kernel has said the child exited, so the
           wait that remains is the moment before its status is collectable. */
        do {
            got = waitpid((pid_t)child->pid, &status, 0);
        } while (got < 0 && errno == EINTR);
    }
    const int result = got < 0 ? -errno : status;

    /* Released before the call, so `exited` may watch this struct again. */
    const inkwell_child_exited exited = child->exited;
    void *const context = child->userdata;
    release(child);
    exited(child, result, context);
    return 0;
}

int inkwell_child_watch(struct inkwell_child *child, struct inkwell_loop *loop, int pid,
                        inkwell_child_exited exited, void *userdata) {
    if (child == NULL || loop == NULL || pid <= 0 || exited == NULL) {
        return -EINVAL;
    }
    if (child->loop != NULL) {
        return -EBUSY;
    }

    /*
     * Ours and not yet reaped, checked without reaping. Neither backend would say: Linux opens a
     * pidfd for any process at all, and Darwin's ESRCH means "a zombie" as readily as "no such
     * process". A watch on a stranger would wait for an exit it can never collect.
     */
    siginfo_t info;
    memset(&info, 0, sizeof info);
    int looked;
    do {
        looked = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
    } while (looked < 0 && errno == EINTR);
    if (looked < 0) {
        return -errno;
    }

    const int fd = watch_open((pid_t)pid);
    if (fd < 0) {
        return fd;
    }
    child->fd = fd;
    child->pid = pid;
    child->exited = exited;
    child->userdata = userdata;
    const int added = inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, on_ready, child);
    if (added < 0) {
        release(child);
        return added;
    }
    child->loop = loop;
    return 0;
}

int inkwell_child_signal(const struct inkwell_child *child, int signal) {
    if (child == NULL) {
        return -EINVAL;
    }
    if (child->loop == NULL) {
        return -ESRCH;
    }
    return watch_signal(child, signal);
}

void inkwell_child_cancel(struct inkwell_child *child) {
    if (child == NULL || child->loop == NULL) {
        return;
    }
    release(child);
}
