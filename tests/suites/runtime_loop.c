#define _POSIX_C_SOURCE 200809L

/* The epoll loop: that it comes up and goes down clean, and that a source which re-arms itself
   faster than the caller's timeout cannot hold it. */

#include "framework/inkwell_test.h"

#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

INKWELL_TEST_CASE(loop_init_shutdown, unit) {
    struct inkwell_loop loop;
    int result = inkwell_loop_init(&loop);
    INKWELL_TEST_FAIL_IF(result < 0, "inkwell_loop_init failed");

    result = inkwell_loop_run(&loop, 0);
    if (result < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "inkwell_loop_run should succeed with zero timeout");
        return;
    }

    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

/*
 * A source that re-arms itself faster than the caller's timeout must not hold the loop.
 *
 * This is the screen progress bar, reduced: an indeterminate meter keeps the UI's 33 ms frame
 * timer armed for as long as it is drawn, and it is drawn for as long as the handshake it
 * reports is unfinished - which only advances in inkwell_transport_registry_tick(), which only
 * runs when this call returns. The loop used to return only on an idle epoll, so the bar
 * starved the work that would have stopped it and the client sat in "sync in progress" for as
 * long as it was left running.
 */
static int rearming_timer_callback(int fd, uint32_t events, void *userdata) {
    (void)events;
    uint64_t expirations = 0U;
    if (read(fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN) {
        return 0;
    }
    *(unsigned *)userdata += 1U;
    struct itimerspec spec = {0};
    spec.it_value.tv_nsec = 2L * 1000000L; /* well inside the run's own timeout */
    (void)timerfd_settime(fd, 0, &spec, NULL);
    return 0;
}

INKWELL_TEST_CASE(loop_run_returns_under_a_hot_source, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");

    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "timerfd_create failed");
        return;
    }
    unsigned firings = 0U;
    struct itimerspec spec = {0};
    spec.it_value.tv_nsec = 2L * 1000000L;
    if (inkwell_loop_add_fd(&loop, fd, EPOLLIN, rearming_timer_callback, &firings) < 0 ||
        timerfd_settime(fd, 0, &spec, NULL) < 0) {
        close(fd);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "could not arm the hot source");
        return;
    }

    const uint64_t started_ms = inkwell_time_monotonic_ms();
    const int result = inkwell_loop_run(&loop, 100);
    const uint64_t elapsed_ms = inkwell_time_monotonic_ms() - started_ms;

    inkwell_loop_remove_fd(&loop, fd);
    close(fd);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(result < 0, "inkwell_loop_run reported an error");
    /* It has to have actually been busy, or the bound proves nothing. */
    INKWELL_TEST_FAIL_IF(firings < 2U, "the hot source did not keep the loop busy");
    INKWELL_TEST_FAIL_IF(elapsed_ms > 1000U,
                         "inkwell_loop_run did not return within its own timeout");
    record_success(test_name);
}
