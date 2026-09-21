#define _POSIX_C_SOURCE 200809L

/* The wake: an eventfd on Linux, a pipe elsewhere, and the same three rules on both. */

#include "framework/inkwell_test.h"

#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/wake.h"

#include <stdint.h>

INKWELL_TEST_CASE(wake_drain_reports_what_was_pending, unit) {
    struct inkwell_wake wake;
    INKWELL_TEST_FAIL_IF(inkwell_wake_open(&wake) < 0, "inkwell_wake_open failed");
    const int idle = inkwell_wake_drain(&wake);
    const int signalled = inkwell_wake_signal(&wake);
    const int pending = inkwell_wake_drain(&wake);
    const int after = inkwell_wake_drain(&wake);
    inkwell_wake_close(&wake);
    INKWELL_TEST_FAIL_IF(idle != 0, "a wake never signalled drained something");
    INKWELL_TEST_FAIL_IF(signalled < 0, "signalling failed");
    INKWELL_TEST_FAIL_IF(pending != 1, "a signalled wake drained nothing");
    INKWELL_TEST_FAIL_IF(after != 0, "one drain did not empty it");
    record_success(test_name);
}

/* Many signals are one wake, and none of them is an error - including the ones past the point
   a pipe fills up, which is where a naive write() would start failing. */
INKWELL_TEST_CASE(wake_many_signals_are_one_drain, unit) {
    struct inkwell_wake wake;
    INKWELL_TEST_FAIL_IF(inkwell_wake_open(&wake) < 0, "inkwell_wake_open failed");
    bool all_ok = true;
    for (int i = 0; i < 100000; ++i) {
        all_ok = all_ok && inkwell_wake_signal(&wake) == 0;
    }
    const int pending = inkwell_wake_drain(&wake);
    const int after = inkwell_wake_drain(&wake);
    inkwell_wake_close(&wake);
    INKWELL_TEST_FAIL_IF(!all_ok, "a repeated signal reported an error");
    INKWELL_TEST_FAIL_IF(pending != 1, "the signals were not pending");
    INKWELL_TEST_FAIL_IF(after != 0, "one drain did not empty it");
    record_success(test_name);
}

struct counted_wake {
    struct inkwell_wake wake;
    int calls;
};

static int count_wake(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct counted_wake *counted = (struct counted_wake *)userdata;
    (void)inkwell_wake_drain(&counted->wake);
    ++counted->calls;
    return 0;
}

INKWELL_TEST_CASE(wake_wakes_the_loop, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    struct counted_wake counted = {{-1, -1}, 0};
    const bool ready =
        inkwell_wake_open(&counted.wake) == 0 &&
        inkwell_loop_add_fd(&loop, counted.wake.fd, INKWELL_LOOP_IN, count_wake, &counted) == 0;
    if (ready) {
        (void)inkwell_loop_run(&loop, 0);
    }
    const int before = counted.calls;
    if (ready) {
        (void)inkwell_wake_signal(&counted.wake);
        (void)inkwell_loop_run(&loop, 0);
    }
    const int woken = counted.calls;
    if (ready) {
        inkwell_loop_remove_fd(&loop, counted.wake.fd);
    }
    inkwell_wake_close(&counted.wake);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(!ready, "could not put the wake on the loop");
    INKWELL_TEST_FAIL_IF(before != 0, "an unsignalled wake woke the loop");
    INKWELL_TEST_FAIL_IF(woken != 1, "a signal did not wake the loop exactly once");
    record_success(test_name);
}

INKWELL_TEST_CASE(wake_close_is_safe_twice, unit) {
    struct inkwell_wake wake;
    INKWELL_TEST_FAIL_IF(inkwell_wake_open(&wake) < 0, "inkwell_wake_open failed");
    inkwell_wake_close(&wake);
    inkwell_wake_close(&wake);
    INKWELL_TEST_FAIL_IF(wake.fd != -1 || wake.write_fd != -1, "close did not clear the ends");
    record_success(test_name);
}
