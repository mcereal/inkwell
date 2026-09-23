#include "inkwell/runtime/timer.h"
#include "inkwell/base/time.h"
#if defined(_WIN32)
#include "windows_handle.h"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#if defined(_WIN32)
#include <limits.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)

#define TIMER_WINDOWS_MAX 64

struct windows_timer {
    int fd;
    HANDLE handle;
    uint32_t period_ms;
    uint64_t due_ms;
};

static struct windows_timer s_timers[TIMER_WINDOWS_MAX];
static bool s_timers_initialized;

static void timers_initialize(void) {
    if (s_timers_initialized) {
        return;
    }
    for (size_t i = 0; i < TIMER_WINDOWS_MAX; ++i) {
        s_timers[i].fd = -1;
    }
    s_timers_initialized = true;
}

static struct windows_timer *timer_find(int fd) {
    timers_initialize();
    for (size_t i = 0; i < TIMER_WINDOWS_MAX; ++i) {
        if (s_timers[i].fd == fd) {
            return &s_timers[i];
        }
    }
    return NULL;
}

static void timer_reset(HANDLE handle) {
    LARGE_INTEGER future;
    future.QuadPart = -1000000000LL;
    (void)SetWaitableTimer(handle, &future, 0, NULL, NULL, FALSE);
    (void)CancelWaitableTimer(handle);
}

int inkwell_timer_open(void) {
    timers_initialize();
    size_t slot = TIMER_WINDOWS_MAX;
    for (size_t i = 0; i < TIMER_WINDOWS_MAX; ++i) {
        if (s_timers[i].fd < 0) {
            slot = i;
            break;
        }
    }
    if (slot == TIMER_WINDOWS_MAX) {
        return -ENOSPC;
    }
    HANDLE handle = CreateWaitableTimerW(NULL, TRUE, NULL);
    if (handle == NULL) {
        return -EIO;
    }
    const int fd = inkwell_windows_handle_register(handle);
    if (fd < 0) {
        (void)CloseHandle(handle);
        return fd;
    }
    s_timers[slot] =
        (struct windows_timer){.fd = fd, .handle = handle, .period_ms = 0U, .due_ms = 0U};
    return fd;
}

void inkwell_timer_close(int fd) {
    struct windows_timer *timer = timer_find(fd);
    if (timer != NULL) {
        timer->fd = -1;
        timer->handle = NULL;
        timer->period_ms = 0U;
        timer->due_ms = 0U;
    }
    if (fd >= 0) {
        inkwell_windows_handle_close(fd);
    }
}

static int timer_set(int fd, uint32_t after_ms, bool repeat) {
    struct windows_timer *timer = timer_find(fd);
    if (timer == NULL) {
        return -EBADF;
    }
    timer->period_ms = repeat ? after_ms : 0U;
    timer->due_ms = after_ms > 0U ? inkwell_time_monotonic_ms() + after_ms : 0U;
    if (after_ms == 0U) {
        timer_reset(timer->handle);
        return 0;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)after_ms * 10000LL;
    return SetWaitableTimer(timer->handle, &due, 0, NULL, NULL, FALSE) != 0 ? 0 : -EIO;
}

int inkwell_timer_disarm(int fd) {
    return timer_set(fd, 0U, false);
}

int64_t inkwell_timer_read(int fd) {
    struct windows_timer *timer = timer_find(fd);
    if (timer == NULL) {
        return -EBADF;
    }
    const DWORD state = WaitForSingleObject(timer->handle, 0);
    if (state == WAIT_TIMEOUT) {
        return 0;
    }
    if (state != WAIT_OBJECT_0) {
        return -EIO;
    }
    if (timer->period_ms > 0U) {
        const uint64_t now_ms = inkwell_time_monotonic_ms();
        uint64_t expirations = 1U;
        if (now_ms > timer->due_ms) {
            expirations += (now_ms - timer->due_ms) / timer->period_ms;
        }
        timer->due_ms += expirations * timer->period_ms;
        const uint64_t until_ms = timer->due_ms > now_ms ? timer->due_ms - now_ms : 1U;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)until_ms * 10000LL;
        if (SetWaitableTimer(timer->handle, &due, 0, NULL, NULL, FALSE) == 0) {
            return -EIO;
        }
        return expirations > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)expirations;
    }
    timer_reset(timer->handle);
    timer->due_ms = 0U;
    return 1;
}

#elif defined(__linux__)
#include <sys/timerfd.h>
#else
#include <fcntl.h>
#include <sys/event.h>
#endif

#if !defined(_WIN32) && defined(__linux__)

int inkwell_timer_open(void) {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

static int timer_set(int fd, uint32_t after_ms, bool repeat) {
    struct itimerspec spec = {0};
    spec.it_value.tv_sec = (time_t)(after_ms / 1000U);
    spec.it_value.tv_nsec = (long)(after_ms % 1000U) * 1000000L;
    if (repeat) {
        spec.it_interval = spec.it_value;
    }
    return timerfd_settime(fd, 0, &spec, NULL) < 0 ? -errno : 0;
}

int inkwell_timer_disarm(int fd) {
    return timer_set(fd, 0U, false);
}

int64_t inkwell_timer_read(int fd) {
    uint64_t expirations = 0;
    for (;;) {
        const ssize_t got = read(fd, &expirations, sizeof expirations);
        if (got == (ssize_t)sizeof expirations) {
            return (int64_t)expirations;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -errno;
        }
        return 0;
    }
}

void inkwell_timer_close(int fd) {
    if (fd >= 0) {
        (void)close(fd);
    }
}

#elif !defined(_WIN32)

/* One timer per kqueue, so the identifier is a constant rather than something to keep. */
#define TIMER_IDENT 1U

int inkwell_timer_open(void) {
    const int fd = kqueue();
    if (fd < 0) {
        return -errno;
    }
    /* kqueue() descriptors are not inherited across fork() on macOS, so FD_CLOEXEC is what is
       left of close-on-exec to ask for; there is no O_NONBLOCK to set, because every wait below
       is given a zero timeout. */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static int timer_set(int fd, uint32_t after_ms, bool repeat) {
    /*
     * EV_DELETE first, and its failure ignored: there is nothing to delete on a timer never armed,
     * and deleting is also what discards an expiry still pending from the old setting. EV_ADD on
     * an existing timer would change its period but leave that expiry queued.
     */
    struct kevent change;
    EV_SET(&change, TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    (void)kevent(fd, &change, 1, NULL, 0, NULL);
    if (after_ms == 0U) {
        return 0;
    }
    /*
     * Milliseconds are Darwin's unit when no NOTE_*SECONDS flag says otherwise. NOTE_CRITICAL
     * because Darwin otherwise coalesces timers by as much as tens of percent to save power, and
     * a frame timer that fires late by a third of a frame is a stutter you can see.
     */
    const unsigned short flags = (unsigned short)(EV_ADD | (repeat ? 0 : EV_ONESHOT));
    EV_SET(&change, TIMER_IDENT, EVFILT_TIMER, flags, NOTE_CRITICAL, (intptr_t)after_ms, NULL);
    return kevent(fd, &change, 1, NULL, 0, NULL) < 0 ? -errno : 0;
}

int inkwell_timer_disarm(int fd) {
    return timer_set(fd, 0U, false);
}

int64_t inkwell_timer_read(int fd) {
    const struct timespec now = {0, 0};
    struct kevent event;
    for (;;) {
        const int got = kevent(fd, NULL, 0, &event, 1, &now);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return -errno;
        }
        if (got == 0) {
            return 0;
        }
        /* An EVFILT_TIMER's data is the number of periods since it was last collected - the same
           count a timerfd read returns - and Darwin keeps counting periods for a one-shot that
           has not been collected yet, where a timerfd says 1. EV_ONESHOT comes back on the event
           and says which this was. */
        if ((event.flags & EV_ONESHOT) != 0U) {
            return 1;
        }
        return (int64_t)event.data;
    }
}

void inkwell_timer_close(int fd) {
    if (fd >= 0) {
        (void)close(fd);
    }
}

#endif

int inkwell_timer_arm_once(int fd, uint32_t after_ms) {
    return timer_set(fd, after_ms, false);
}

int inkwell_timer_arm_every(int fd, uint32_t period_ms) {
    return timer_set(fd, period_ms, true);
}
