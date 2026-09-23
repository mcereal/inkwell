#define _POSIX_C_SOURCE 200809L

#include "inkwell/runtime/signals.h"

#include "inkwell/base/log.h"

#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/signalfd.h>
#else
#include <fcntl.h>
#include <sys/event.h>
#endif

static const int k_signals[] = {SIGINT, SIGTERM, SIGHUP};
#define SIGNAL_COUNT (sizeof k_signals / sizeof k_signals[0])

#if defined(__linux__)

/* The signal mask is process-wide, so the saved copy is file-static rather than per-instance.
   Keeping sigset_t out of signals.h also spares every includer the POSIX feature macros. */
static sigset_t s_saved_mask;
static bool s_mask_saved;

static void inkwell_signals_restore_mask(void) {
    if (!s_mask_saved) {
        return;
    }
    if (sigprocmask(SIG_SETMASK, &s_saved_mask, NULL) < 0) {
        inkwell_log_warn("signals", "Failed to restore signal mask: %s", strerror(errno));
    }
    s_mask_saved = false;
}

static int inkwell_signals_event_callback(int fd, uint32_t events, void *userdata) {
    struct inkwell_signals *signals = (struct inkwell_signals *)userdata;
    if (signals == NULL || (events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }

    struct signalfd_siginfo info;
    for (;;) {
        const ssize_t bytes = read(fd, &info, sizeof info);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                inkwell_log_warn("signals", "signalfd read failed: %s", strerror(errno));
            }
            break;
        }
        if (bytes != (ssize_t)sizeof info) {
            break;
        }

        inkwell_log_info("signals", "Received signal %u; shutting down", info.ssi_signo);
        inkwell_loop_request_stop(signals->loop);
        break;
    }

    return 0;
}

/* The descriptor that reports the signals, or a negative errno with the process as it was. */
static int signals_open(void) {
    sigset_t mask;
    sigemptyset(&mask);
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        sigaddset(&mask, k_signals[i]);
    }

    /* The signals must be blocked for signalfd to receive them rather than the default
       disposition running first. Every failure path below has to put the mask back: leaving
       them blocked with no signalfd to read them would make the process ignore SIGTERM. */
    if (sigprocmask(SIG_BLOCK, &mask, &s_saved_mask) < 0) {
        inkwell_log_warn("signals", "sigprocmask failed: %s", strerror(errno));
        return -errno;
    }
    s_mask_saved = true;

    const int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        inkwell_log_warn("signals", "signalfd failed: %s", strerror(errno));
        const int saved_errno = errno;
        inkwell_signals_restore_mask();
        return -saved_errno;
    }
    return fd;
}

static void signals_restore(void) {
    inkwell_signals_restore_mask();
}

#else

/*
 * kqueue's EVFILT_SIGNAL, on macOS.
 *
 * It records every attempt to deliver a signal, including one whose disposition is SIG_IGN, and
 * that is how it is used: the three are ignored rather than blocked, so the default action never
 * runs and the kqueue still hears them. Blocking would work as well for hearing them, and would
 * leave each one pending - so the moment shutdown put the mask back, the SIGINT that started the
 * shutdown would be delivered for real and kill the process halfway through it. signalfd consumes
 * the signal it reports; this does not, so the signal must not be allowed to pend.
 */
static struct sigaction s_saved_actions[SIGNAL_COUNT];
static bool s_actions_saved;

static void signals_restore(void) {
    if (!s_actions_saved) {
        return;
    }
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        if (sigaction(k_signals[i], &s_saved_actions[i], NULL) < 0) {
            inkwell_log_warn("signals", "Failed to restore signal %d: %s", k_signals[i],
                             strerror(errno));
        }
    }
    s_actions_saved = false;
}

static int inkwell_signals_event_callback(int fd, uint32_t events, void *userdata) {
    struct inkwell_signals *signals = (struct inkwell_signals *)userdata;
    if (signals == NULL || (events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }
    const struct timespec now = {0, 0};
    struct kevent event;
    if (kevent(fd, NULL, 0, &event, 1, &now) == 1) {
        inkwell_log_info("signals", "Received signal %u; shutting down", (unsigned)event.ident);
        inkwell_loop_request_stop(signals->loop);
    }
    return 0;
}

static int signals_open(void) {
    const int fd = kqueue();
    if (fd < 0) {
        inkwell_log_warn("signals", "kqueue failed: %s", strerror(errno));
        return -errno;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct kevent changes[SIGNAL_COUNT];
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        EV_SET(&changes[i], (uintptr_t)k_signals[i], EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    }
    if (kevent(fd, changes, (int)SIGNAL_COUNT, NULL, 0, NULL) < 0) {
        const int saved_errno = errno;
        inkwell_log_warn("signals", "watching signals failed: %s", strerror(saved_errno));
        close(fd);
        return -saved_errno;
    }

    /* Only once the kqueue is listening, so there is no moment in which a signal is ignored and
       nobody hears it. */
    struct sigaction ignore;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    for (size_t i = 0; i < SIGNAL_COUNT; ++i) {
        (void)sigaction(k_signals[i], &ignore, &s_saved_actions[i]);
    }
    s_actions_saved = true;
    return fd;
}

#endif

int inkwell_signals_init(struct inkwell_signals *signals, struct inkwell_loop *loop) {
    if (signals == NULL || loop == NULL) {
        return -EINVAL;
    }

    signals->loop = loop;
    signals->fd = -1;

    const int fd = signals_open();
    if (fd < 0) {
        return fd;
    }
    signals->fd = fd;

    const int add_result = inkwell_loop_add_fd(loop, signals->fd, INKWELL_LOOP_IN,
                                               inkwell_signals_event_callback, signals);
    if (add_result < 0) {
        inkwell_log_warn("signals", "Failed to watch signalfd: %d", add_result);
        close(signals->fd);
        signals->fd = -1;
        signals_restore();
        return add_result;
    }

    return 0;
}

void inkwell_signals_shutdown(struct inkwell_signals *signals) {
    if (signals == NULL) {
        return;
    }

    if (signals->fd >= 0) {
        if (signals->loop != NULL) {
            inkwell_loop_remove_fd(signals->loop, signals->fd);
        }
        close(signals->fd);
        signals->fd = -1;
    }

    signals_restore();
    signals->loop = NULL;
}
