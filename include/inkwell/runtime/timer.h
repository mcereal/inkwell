#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A timer that is a descriptor, so it waits on the loop like everything else.
 *
 * On Linux this is a timerfd on CLOCK_MONOTONIC and every call is the one it replaces. On macOS it
 * is a kqueue holding one EVFILT_TIMER: a kqueue is itself
 * a descriptor that polls readable while it has an event pending, so the loop's kqueue can wait
 * on it exactly as epoll waits on a timerfd. Windows registers a waitable timer under an integer
 * source key so callers retain the same platform-neutral interface.
 *
 * Two shapes rather than timerfd's full itimerspec, because they are the two anything here has
 * ever asked for: once after a delay, and every period with the first expiry one period out. A
 * kqueue timer has no separate first delay, and an interface that offered one would be a promise
 * one of the two systems keeps by emulation.
 *
 * Arming replaces whatever was armed, and anything pending from it. That is timerfd_settime()'s
 * rule and the one the callers rely on: rearming a frame timer must not let a stale expiry through.
 *
 * inkwell_timer_read() is the read() of a timerfd: how many times it has expired since the last
 * read, 0 when it has not, or a negative errno. A callback must call it - or the descriptor stays
 * readable and the loop spins, which is the same rule a timerfd has.
 */

/* A disarmed timer: its descriptor, or a negative errno. */
int inkwell_timer_open(void);
/* Releases the timer on every platform. Use this rather than close(), which cannot release the
   small amount of Windows-side bookkeeping associated with a waitable timer. */
void inkwell_timer_close(int fd);

/* Expire once, `after_ms` from now. 0 is the same as disarming. 0 or a negative errno. */
int inkwell_timer_arm_once(int fd, uint32_t after_ms);

/* Expire every `period_ms`, the first time one period from now. 0 disarms. */
int inkwell_timer_arm_every(int fd, uint32_t period_ms);

int inkwell_timer_disarm(int fd);

int64_t inkwell_timer_read(int fd);

#ifdef __cplusplus
}
#endif
