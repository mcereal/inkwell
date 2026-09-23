#define _POSIX_C_SOURCE 200809L

/* The loop: that it comes up and goes down clean, that a source which re-arms itself faster than
   the caller's timeout cannot hold it, and that what a callback is told is what epoll would have
   told it - on kqueue as well, where readable and writable arrive as two events and end-of-file is
   a flag rather than an event. */

#include "framework/inkwell_test.h"

#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
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
    if (inkwell_timer_read(fd) < 0) {
        return 0;
    }
    *(unsigned *)userdata += 1U;
    (void)inkwell_timer_arm_once(fd, 2U); /* well inside the run's own timeout */
    return 0;
}

INKWELL_TEST_CASE(loop_run_returns_under_a_hot_source, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");

    const int fd = inkwell_timer_open();
    if (fd < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "inkwell_timer_open failed");
        return;
    }
    unsigned firings = 0U;
    if (inkwell_loop_add_fd(&loop, fd, INKWELL_LOOP_IN, rearming_timer_callback, &firings) < 0 ||
        inkwell_timer_arm_once(fd, 2U) < 0) {
        inkwell_timer_close(fd);
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "could not arm the hot source");
        return;
    }

    const uint64_t started_ms = inkwell_time_monotonic_ms();
    const int result = inkwell_loop_run(&loop, 100);
    const uint64_t elapsed_ms = inkwell_time_monotonic_ms() - started_ms;

    inkwell_loop_remove_fd(&loop, fd);
    inkwell_timer_close(fd);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(result < 0, "inkwell_loop_run reported an error");
    /* It has to have actually been busy, or the bound proves nothing. */
    INKWELL_TEST_FAIL_IF(firings < 2U, "the hot source did not keep the loop busy");
    INKWELL_TEST_FAIL_IF(elapsed_ms > 1000U,
                         "inkwell_loop_run did not return within its own timeout");
    record_success(test_name);
}

/*
 * What one run told one source: how many times it was called and the union of what it heard.
 *
 * Every source here is level-triggered and stays ready - a socket is always writable, a hung-up
 * pipe is always hung up - and inkwell_loop_run(0) drains until nothing is ready, so each callback
 * quiets its own source or the run never ends. `then` is how: a new mask, or removal. Removal is
 * the only quiet there is for a hangup, which epoll reports whatever the mask says.
 */
#define THEN_REMOVE UINT32_MAX

struct heard {
    struct inkwell_loop *loop;
    uint32_t then;
    unsigned calls;
    uint32_t events;
};

static int record_events(int fd, uint32_t events, void *userdata) {
    struct heard *heard = (struct heard *)userdata;
    heard->calls += 1U;
    heard->events |= events;
    if (heard->then == THEN_REMOVE) {
        (void)inkwell_loop_remove_fd(heard->loop, fd);
    } else {
        (void)inkwell_loop_update_fd(heard->loop, fd, heard->then);
    }
    return 0;
}

/*
 * A descriptor that is readable and writable at once is one callback, not two.
 *
 * epoll gives that for nothing. kqueue reports the read filter and the write filter as separate
 * events, and a backend that dispatched them as they came would call a stream's handler twice in
 * one turn - the second time with half the mask, which is a handler deciding the socket is no
 * longer readable when it is. The callback masks the source down to nothing, so an unmerged
 * second event would still arrive and be counted.
 */
INKWELL_TEST_CASE(loop_readable_and_writable_is_one_callback, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "socketpair failed");
        return;
    }
    const char byte = 'x';
    struct heard heard = {&loop, 0U, 0U, 0U};
    const bool ready = write(pair[1], &byte, 1) == 1 &&
                       inkwell_loop_add_fd(&loop, pair[0], INKWELL_LOOP_IN | INKWELL_LOOP_OUT,
                                           record_events, &heard) == 0;
    if (ready) {
        (void)inkwell_loop_run(&loop, 0);
        inkwell_loop_remove_fd(&loop, pair[0]);
    }
    close(pair[0]);
    close(pair[1]);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!ready, "could not set the pair up");
    INKWELL_TEST_FAIL_IF(heard.calls != 1U, "one ready descriptor was called more than once");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_IN) == 0U, "readable was not reported");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_OUT) == 0U, "writable was not reported");
    record_success(test_name);
}

/* Changing a source's mask changes what wakes it: kqueue's half of this is a diff of filters, and
   a filter left behind or not added is a source that goes deaf or never goes quiet. */
INKWELL_TEST_CASE(loop_update_changes_what_is_heard, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "socketpair failed");
        return;
    }
    /* Each callback drops writable again, so what the third run hears is whether it stuck. */
    struct heard heard = {&loop, INKWELL_LOOP_IN, 0U, 0U};
    struct heard quiet = {0};
    struct heard writable = {0};
    struct heard dropped = {0};
    bool ok = inkwell_loop_add_fd(&loop, pair[0], INKWELL_LOOP_IN, record_events, &heard) == 0;
    if (ok) {
        /* Nothing to read, and not asking about writing: nothing to say. */
        (void)inkwell_loop_run(&loop, 0);
        quiet = heard;
        heard.calls = 0U;
        heard.events = 0U;
        ok = inkwell_loop_update_fd(&loop, pair[0], INKWELL_LOOP_IN | INKWELL_LOOP_OUT) == 0;
    }
    if (ok) {
        (void)inkwell_loop_run(&loop, 0);
        writable = heard;
        heard.calls = 0U;
        heard.events = 0U;
        (void)inkwell_loop_run(&loop, 0);
        dropped = heard;
    }
    inkwell_loop_remove_fd(&loop, pair[0]);
    close(pair[0]);
    close(pair[1]);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!ok, "could not add or update the source");
    INKWELL_TEST_FAIL_IF(quiet.calls != 0U, "an idle source was called");
    INKWELL_TEST_FAIL_IF((writable.events & INKWELL_LOOP_OUT) == 0U,
                         "asking for writable did not report it");
    INKWELL_TEST_FAIL_IF(dropped.calls != 0U, "writable was still reported after it was dropped");
    record_success(test_name);
}

/* The writer going away is a hangup. The resolver reads its child's answer this way: the pipe
   closing is how it knows the child has said everything it will. */
INKWELL_TEST_CASE(loop_closed_writer_is_a_hangup, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    int fds[2];
    if (pipe(fds) < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "pipe failed");
        return;
    }
    close(fds[1]);
    struct heard heard = {&loop, THEN_REMOVE, 0U, 0U};
    const bool added =
        inkwell_loop_add_fd(&loop, fds[0], INKWELL_LOOP_IN, record_events, &heard) == 0;
    if (added) {
        (void)inkwell_loop_run(&loop, 0);
    }
    close(fds[0]);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!added, "could not add the pipe");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_HUP) == 0U,
                         "a pipe with no writer was not reported as a hangup");
    record_success(test_name);
}

/* A socket whose peer has closed but left bytes behind is readable and not yet a hangup, so a
   caller that looks for a hangup before it reads cannot lose the last of what was sent. */
INKWELL_TEST_CASE(loop_peer_close_with_data_left_is_readable, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        inkwell_loop_shutdown(&loop);
        record_failure(test_name, "socketpair failed");
        return;
    }
    const char byte = 'x';
    struct heard heard = {&loop, THEN_REMOVE, 0U, 0U};
    const bool ready =
        write(pair[1], &byte, 1) == 1 && shutdown(pair[1], SHUT_WR) == 0 &&
        inkwell_loop_add_fd(&loop, pair[0], INKWELL_LOOP_IN, record_events, &heard) == 0;
    if (ready) {
        (void)inkwell_loop_run(&loop, 0);
    }
    close(pair[0]);
    close(pair[1]);
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!ready, "could not set the pair up");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_IN) == 0U, "the last byte was not readable");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_HUP) != 0U,
                         "a half-closed socket with data left was reported as a hangup");
    record_success(test_name);
}

/*
 * A connection reset under a source that only reads is an error, as epoll reports it. kqueue
 * carries the socket's error in fflags on whichever filter saw the end of file, and the read
 * filter used to drop it - so a reader was told "readable, hung up" about a reset and could not
 * tell it from an orderly close.
 */
INKWELL_TEST_CASE(loop_reset_under_a_reader_is_an_error, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    int server = -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t address_len = sizeof address;
    bool ready = listener >= 0 && client >= 0 &&
                 bind(listener, (struct sockaddr *)&address, sizeof address) == 0 &&
                 listen(listener, 1) == 0 &&
                 getsockname(listener, (struct sockaddr *)&address, &address_len) == 0 &&
                 connect(client, (struct sockaddr *)&address, sizeof address) == 0;
    if (ready) {
        server = accept(listener, NULL, NULL);
        /* A zero linger turns the close into a reset rather than a FIN. */
        const struct linger abort_close = {1, 0};
        ready = server >= 0 &&
                setsockopt(server, SOL_SOCKET, SO_LINGER, &abort_close, sizeof abort_close) == 0;
    }
    struct heard heard = {&loop, THEN_REMOVE, 0U, 0U};
    if (ready) {
        close(server);
        server = -1;
        ready = inkwell_loop_add_fd(&loop, client, INKWELL_LOOP_IN, record_events, &heard) == 0;
    }
    /* Bounded rather than one turn: macOS hands loopback segments to an input thread. */
    const uint64_t give_up = inkwell_time_monotonic_ms() + 1000U;
    while (ready && heard.calls == 0U && inkwell_time_monotonic_ms() < give_up) {
        (void)inkwell_loop_run(&loop, 20);
    }
    if (server >= 0) {
        close(server);
    }
    if (client >= 0) {
        close(client);
    }
    if (listener >= 0) {
        close(listener);
    }
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!ready, "could not set the connection up");
    INKWELL_TEST_FAIL_IF(heard.calls == 0U, "the reset was never reported");
    INKWELL_TEST_FAIL_IF((heard.events & INKWELL_LOOP_ERR) == 0U,
                         "a reset seen by a reader was not reported as an error");
    record_success(test_name);
}

/* A stop requested from outside any callback still ends the run: the loop's own wake is what
   carries it, and on macOS that is a pipe rather than an eventfd. */
static int request_stop_on_timer(int fd, uint32_t events, void *userdata) {
    (void)events;
    (void)inkwell_timer_read(fd);
    inkwell_loop_request_stop((struct inkwell_loop *)userdata);
    return 0;
}

INKWELL_TEST_CASE(loop_request_stop_ends_an_unbounded_run, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) < 0, "inkwell_loop_init failed");
    const int fd = inkwell_timer_open();
    const bool armed =
        fd >= 0 &&
        inkwell_loop_add_fd(&loop, fd, INKWELL_LOOP_IN, request_stop_on_timer, &loop) == 0 &&
        inkwell_timer_arm_once(fd, 5U) == 0;
    const uint64_t started_ms = inkwell_time_monotonic_ms();
    const int result = armed ? inkwell_loop_run(&loop, -1) : -1;
    const uint64_t elapsed_ms = inkwell_time_monotonic_ms() - started_ms;
    if (fd >= 0) {
        inkwell_loop_remove_fd(&loop, fd);
        inkwell_timer_close(fd);
    }
    const bool stopped = loop.stop_requested;
    inkwell_loop_shutdown(&loop);

    INKWELL_TEST_FAIL_IF(!armed, "could not arm the timer");
    INKWELL_TEST_FAIL_IF(result < 0, "inkwell_loop_run reported an error");
    INKWELL_TEST_FAIL_IF(!stopped, "the stop was not recorded");
    INKWELL_TEST_FAIL_IF(elapsed_ms > 1000U, "the run did not end when asked to");
    record_success(test_name);
}

struct socket_watch_probe {
    int token;
    char byte;
};

static int socket_watch_callback(int token, uint32_t events, void *userdata) {
    struct socket_watch_probe *probe = (struct socket_watch_probe *)userdata;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        probe->token = token;
        (void)recv(token, &probe->byte, 1, 0);
    }
    return 0;
}

INKWELL_TEST_CASE(loop_watches_native_socket_with_token, unit) {
    int sockets[2] = {-1, -1};
    INKWELL_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0,
                         "socket pair should open");
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        (void)close(sockets[0]);
        (void)close(sockets[1]);
        record_failure(test_name, "loop should initialize");
        return;
    }
    struct socket_watch_probe probe = {.token = -1};
    int token = -1;
    const int watched = inkwell_loop_watch_socket(&loop, (uintptr_t)sockets[0], INKWELL_LOOP_IN,
                                                  socket_watch_callback, &probe, &token);
    const int ran =
        watched == 0 && send(sockets[1], "x", 1, 0) == 1 ? inkwell_loop_run(&loop, 100) : -1;
    if (watched == 0) {
        (void)inkwell_loop_remove_fd(&loop, token);
    }
    inkwell_loop_shutdown(&loop);
    (void)close(sockets[0]);
    (void)close(sockets[1]);
    INKWELL_TEST_FAIL_IF(watched != 0 || ran != 0 || token != probe.token || probe.byte != 'x',
                         "socket callback should receive its registration token");
    record_success(test_name);
}
