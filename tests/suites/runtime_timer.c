#define _POSIX_C_SOURCE 200809L

/*
 * The timer descriptor: a timerfd on Linux, a kqueue holding one EVFILT_TIMER on macOS.
 *
 * These are the timerfd rules the callers were written against, checked on whichever of the two
 * this was built for - so the macOS emulation is held to the kernel's behaviour rather than to a
 * reading of it.
 */

#include "framework/inkwell_test.h"

#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include <stdint.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000L, (ms % 1000L) * 1000000L};
    while (nanosleep(&pause, &pause) != 0) {
    }
}

INKWELL_TEST_CASE(timer_unarmed_reads_nothing, unit) {
    const int fd = inkwell_timer_open();
    INKWELL_TEST_FAIL_IF(fd < 0, "inkwell_timer_open failed");
    const int64_t read = inkwell_timer_read(fd);
    inkwell_timer_close(fd);
    INKWELL_TEST_FAIL_IF(read != 0, "a timer never armed reported an expiry");
    record_success(test_name);
}

INKWELL_TEST_CASE(timer_once_expires_once, unit) {
    const int fd = inkwell_timer_open();
    INKWELL_TEST_FAIL_IF(fd < 0, "inkwell_timer_open failed");
    const int armed = inkwell_timer_arm_once(fd, 2U);
    sleep_ms(20);
    const int64_t first = inkwell_timer_read(fd);
    sleep_ms(10);
    const int64_t second = inkwell_timer_read(fd);
    inkwell_timer_close(fd);
    INKWELL_TEST_FAIL_IF(armed < 0, "arming failed");
    INKWELL_TEST_FAIL_IF(first != 1, "a one-shot did not expire exactly once");
    INKWELL_TEST_FAIL_IF(second != 0, "a one-shot expired again");
    record_success(test_name);
}

INKWELL_TEST_CASE(timer_every_keeps_expiring, unit) {
    const int fd = inkwell_timer_open();
    INKWELL_TEST_FAIL_IF(fd < 0, "inkwell_timer_open failed");
    const int armed = inkwell_timer_arm_every(fd, 2U);
    sleep_ms(30);
    const int64_t collected = inkwell_timer_read(fd);
    sleep_ms(30);
    const int64_t later = inkwell_timer_read(fd);
    inkwell_timer_close(fd);
    INKWELL_TEST_FAIL_IF(armed < 0, "arming failed");
    /* A count, as a timerfd read is: several periods passed, and they are one read. */
    INKWELL_TEST_FAIL_IF(collected < 2, "a periodic timer did not count its expiries");
    INKWELL_TEST_FAIL_IF(later < 1, "a periodic timer stopped after one read");
    record_success(test_name);
}

/*
 * Re-arming throws away an expiry that has not been read. A frame timer is re-armed every frame,
 * and the one thing it may not do is let a stale expiry through - that is a frame drawn at the old
 * rate after the caller asked for the new one.
 */
INKWELL_TEST_CASE(timer_rearm_discards_a_pending_expiry, unit) {
    const int fd = inkwell_timer_open();
    INKWELL_TEST_FAIL_IF(fd < 0, "inkwell_timer_open failed");
    const int armed = inkwell_timer_arm_once(fd, 1U);
    sleep_ms(20);
    const int rearmed = inkwell_timer_arm_once(fd, 10000U);
    const int64_t read = inkwell_timer_read(fd);
    inkwell_timer_close(fd);
    INKWELL_TEST_FAIL_IF(armed < 0 || rearmed < 0, "arming failed");
    INKWELL_TEST_FAIL_IF(read != 0, "an expiry from before the re-arm survived it");
    record_success(test_name);
}

INKWELL_TEST_CASE(timer_disarm_stops_it, unit) {
    const int fd = inkwell_timer_open();
    INKWELL_TEST_FAIL_IF(fd < 0, "inkwell_timer_open failed");
    const int armed = inkwell_timer_arm_every(fd, 2U);
    const int disarmed = inkwell_timer_disarm(fd);
    sleep_ms(20);
    const int64_t read = inkwell_timer_read(fd);
    inkwell_timer_close(fd);
    INKWELL_TEST_FAIL_IF(armed < 0 || disarmed < 0, "arming or disarming failed");
    INKWELL_TEST_FAIL_IF(read != 0, "a disarmed timer expired");
    record_success(test_name);
}

static int count_expiry(int fd, uint32_t events, void *userdata) {
    (void)events;
    const int64_t expired = inkwell_timer_read(fd);
    if (expired > 0) {
        *(int64_t *)userdata += expired;
    }
    return 0;
}

/* The point of the descriptor: the loop wakes for it. */
INKWELL_TEST_CASE(timer_wakes_the_loop, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    const int fd = inkwell_timer_open();
    int64_t expired = 0;
    const bool armed =
        fd >= 0 && inkwell_loop_add_fd(&loop, fd, INKWELL_LOOP_IN, count_expiry, &expired) == 0 &&
        inkwell_timer_arm_once(fd, 5U) == 0;
    const uint64_t deadline = inkwell_time_monotonic_ms() + 1000U;
    while (armed && expired == 0 && inkwell_time_monotonic_ms() < deadline) {
        (void)inkwell_loop_run(&loop, 50);
    }
    if (fd >= 0) {
        inkwell_loop_remove_fd(&loop, fd);
        inkwell_timer_close(fd);
    }
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(!armed, "could not arm the timer on the loop");
    INKWELL_TEST_FAIL_IF(expired != 1, "the loop did not report the expiry");
    record_success(test_name);
}
