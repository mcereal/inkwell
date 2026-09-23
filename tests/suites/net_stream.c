/* The byte stream: the queue the caller owns, and what it says when it cannot send. */

#include "framework/inkwell_test.h"

#include "inkwell/net/stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* What a sink saw, for a case to assert against. */
struct stream_probe {
    uint8_t seen[512];
    size_t seen_len;
    uint32_t dropped[8];
    size_t dropped_len;
};

static void probe_bytes(void *userdata, const uint8_t *bytes, size_t len) {
    struct stream_probe *probe = (struct stream_probe *)userdata;
    for (size_t i = 0; i < len && probe->seen_len < sizeof probe->seen; ++i) {
        probe->seen[probe->seen_len++] = bytes[i];
    }
}

static void probe_dropped(void *userdata, uint32_t id) {
    struct stream_probe *probe = (struct stream_probe *)userdata;
    if (probe->dropped_len < sizeof probe->dropped / sizeof probe->dropped[0]) {
        probe->dropped[probe->dropped_len++] = id;
    }
}

/* A non-blocking socket pair, which is the only kind of descriptor a case here needs: it is a
   socket, so it exercises the MSG_NOSIGNAL path, and both ends are in the test. */
static bool probe_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
    }
    return true;
}

static int stream_test_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    (void)userdata;
    return 0;
}

INKWELL_TEST_CASE(stream_file_watch_uses_descriptor_for_updates_and_close, unit) {
    const char *failure = NULL;
    int fds[2];
    INKWELL_TEST_FAIL_IF(!probe_pair(fds), "could not make a socket pair");

    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        record_failure(test_name, "loop should initialize");
        return;
    }
    struct inkwell_stream_slot slots[1];
    uint8_t queue[1];
    struct inkwell_stream stream;
    (void)inkwell_stream_init(&stream, "test", slots, 1U, queue, sizeof queue);
    if (inkwell_stream_open(&stream, fds[0], INKWELL_STREAM_FILE, &loop, stream_test_ready, NULL) !=
        0) {
        failure = "file descriptor should register";
        goto done;
    }

    /* Fill the outgoing buffer so send() must arm OUT on the registered descriptor. */
    uint8_t fill[4096] = {0};
    while (write(fds[0], fill, sizeof fill) > 0) {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        failure = "nonblocking socket should fill";
        goto done;
    }
    const uint8_t byte = 1U;
    if (inkwell_stream_send(&stream, &byte, 1U, 0U) != 0 || inkwell_stream_queued(&stream) != 1U) {
        failure = "blocked write should stay queued";
        goto done;
    }
    bool out_armed = false;
    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (loop.sources[i].active && loop.sources[i].fd == fds[0]) {
            out_armed = (loop.sources[i].events & INKWELL_LOOP_OUT) != 0U;
        }
    }
    if (!out_armed) {
        failure = "OUT should be armed on the file descriptor";
        goto done;
    }
done:
    if (inkwell_stream_is_open(&stream)) {
        inkwell_stream_close(&stream);
        if (inkwell_loop_remove_fd(&loop, fds[0]) != -ENOENT && failure == NULL) {
            failure = "close should remove the file descriptor watch";
        }
    } else {
        (void)close(fds[0]);
    }
    (void)close(fds[1]);
    inkwell_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

INKWELL_TEST_CASE(stream_round_trips_bytes, unit) {
    int fds[2];
    INKWELL_TEST_FAIL_IF(!probe_pair(fds), "could not make a socket pair");

    struct inkwell_stream_slot slots[2];
    uint8_t queue[2 * 8];
    struct inkwell_stream stream;
    struct stream_probe probe;
    memset(&probe, 0, sizeof probe);

    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_init(&stream, "test", slots, 2U, queue, 8U) != 0,
                                 (void)close(fds[0]);
                                 (void)close(fds[1]), "init failed");
    inkwell_stream_set_sink(&stream, probe_bytes, probe_dropped, &probe);
    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_stream_open_socket(&stream, (inkwell_socket)fds[0], NULL, NULL, NULL) != 0,
        (void)close(fds[0]);
        (void)close(fds[1]), "open failed");

    /* Out through the stream, in through the other end of the pair. */
    const uint8_t payload[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_send(&stream, payload, sizeof payload, 7U) != 0,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "send failed");
    uint8_t got[16];
    const ssize_t read_back = read(fds[1], got, sizeof got);
    INKWELL_TEST_FAIL_IF_CLEANUP(
        read_back != (ssize_t)sizeof payload || memcmp(got, payload, sizeof payload) != 0,
        inkwell_stream_close(&stream);
        (void)close(fds[1]), "the bytes on the wire should be exactly what was sent");
    /* Nothing was framed on the way out: this layer adds no header and no sentinel, which is
       what lets the owner choose a wire format. */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_stream_queued(&stream) != 0U, inkwell_stream_close(&stream);
        (void)close(fds[1]), "a write the descriptor took should leave the queue empty");

    /* And in the other direction, through pump(). */
    const uint8_t inbound[] = {1U, 2U, 3U};
    INKWELL_TEST_FAIL_IF_CLEANUP(write(fds[1], inbound, sizeof inbound) != (ssize_t)sizeof inbound,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "could not write to the far end");
    const int pumped = inkwell_stream_pump(&stream);
    INKWELL_TEST_FAIL_IF_CLEANUP(pumped != (int)sizeof inbound, inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "pump should report what it read");
    INKWELL_TEST_FAIL_IF_CLEANUP(probe.seen_len != sizeof inbound ||
                                     memcmp(probe.seen, inbound, sizeof inbound) != 0,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "the sink should see the bytes unchanged");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_bytes_received(&stream) != sizeof inbound,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "and they should be counted");

    inkwell_stream_close(&stream);
    (void)close(fds[1]);
    record_success(test_name);
}

INKWELL_TEST_CASE(stream_refuses_what_the_callers_budget_will_not_hold, unit) {
    /*
     * The whole point of the component: the queue's depth and its slot size are the caller's
     * two numbers, and the stream refuses rather than deciding either of them for itself. A
     * platform layer that picked them would be one protocol's largest message and one
     * application's patience compiled into everybody else's.
     */
    int fds[2];
    INKWELL_TEST_FAIL_IF(!probe_pair(fds), "could not make a socket pair");

    struct inkwell_stream_slot slots[2];
    uint8_t queue[2 * 4];
    struct inkwell_stream stream;
    struct stream_probe probe;
    memset(&probe, 0, sizeof probe);
    (void)inkwell_stream_init(&stream, "test", slots, 2U, queue, 4U);
    inkwell_stream_set_sink(&stream, probe_bytes, probe_dropped, &probe);
    (void)inkwell_stream_open(&stream, fds[0], INKWELL_STREAM_SOCKET, NULL, NULL, NULL);

    /* Too large for a slot: refused, not truncated. */
    const uint8_t oversized[5] = {0U};
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_send(&stream, oversized, sizeof oversized, 0U) !=
                                     -EMSGSIZE,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "a write larger than a slot is -EMSGSIZE");

    /* Fill the pair's buffer so nothing drains, then fill the queue behind it. */
    uint8_t filler[4096];
    memset(filler, 0xAAU, sizeof filler);
    while (send(fds[0], filler, sizeof filler, MSG_NOSIGNAL) > 0) {
    }

    const uint8_t four[4] = {9U, 9U, 9U, 9U};
    int full_at = -1;
    for (int i = 0; i < 4; ++i) {
        if (inkwell_stream_send(&stream, four, sizeof four, (uint32_t)(i + 1)) == -ENOSPC) {
            full_at = i;
            break;
        }
    }
    INKWELL_TEST_FAIL_IF_CLEANUP(full_at != 2, inkwell_stream_close(&stream);
                                 (void)close(fds[1]),
                                 "a two-slot queue should take two writes and refuse the third");

    /*
     * And closing owes an answer for each one it accepted and never sent - in the order they
     * were queued, and only for the ones that were named.
     */
    inkwell_stream_close(&stream);
    INKWELL_TEST_FAIL_IF_CLEANUP(
        probe.dropped_len != 2U || probe.dropped[0] != 1U || probe.dropped[1] != 2U,
        (void)close(fds[1]), "every queued write should be reported dropped, in order");
    (void)close(fds[1]);
    record_success(test_name);
}

INKWELL_TEST_CASE(stream_without_a_queue_still_reads, unit) {
    /* A caller that only listens passes no queue at all, which is a configuration rather than a
       mistake: it reads, and every send is refused rather than reaching a NULL. */
    int fds[2];
    INKWELL_TEST_FAIL_IF(!probe_pair(fds), "could not make a socket pair");

    struct inkwell_stream stream;
    struct stream_probe probe;
    memset(&probe, 0, sizeof probe);
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_init(&stream, "test", NULL, 0U, NULL, 0U) != 0,
                                 (void)close(fds[0]);
                                 (void)close(fds[1]), "init failed");
    inkwell_stream_set_sink(&stream, probe_bytes, NULL, &probe);
    (void)inkwell_stream_open(&stream, fds[0], INKWELL_STREAM_SOCKET, NULL, NULL, NULL);

    const uint8_t byte = 0x42U;
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_send(&stream, &byte, 1U, 0U) != -ENOSPC,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "a stream with no queue refuses every send");
    INKWELL_TEST_FAIL_IF_CLEANUP(write(fds[1], &byte, 1U) != 1, inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "could not write to the far end");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_pump(&stream) != 1 || probe.seen_len != 1U ||
                                     probe.seen[0] != 0x42U,
                                 inkwell_stream_close(&stream);
                                 (void)close(fds[1]), "but it still reads");

    inkwell_stream_close(&stream);
    (void)close(fds[1]);
    record_success(test_name);
}

INKWELL_TEST_CASE(stream_reports_a_far_end_that_went_away, unit) {
    /*
     * EOF is -ENOTCONN and does not close the stream. Two things depend on that: the owner is
     * the one that knows what a vanished peer means, and it is also the one holding the
     * descriptor's identity - closing here would have it reported twice.
     */
    int fds[2];
    INKWELL_TEST_FAIL_IF(!probe_pair(fds), "could not make a socket pair");

    struct inkwell_stream stream;
    (void)inkwell_stream_init(&stream, "test", NULL, 0U, NULL, 0U);
    (void)inkwell_stream_open(&stream, fds[0], INKWELL_STREAM_SOCKET, NULL, NULL, NULL);
    (void)close(fds[1]);

    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_stream_pump(&stream) != -ENOTCONN,
                                 inkwell_stream_close(&stream), "a closed far end is -ENOTCONN");
    INKWELL_TEST_FAIL_IF_CLEANUP(!inkwell_stream_is_open(&stream), inkwell_stream_close(&stream),
                                 "and the stream stays open until its owner says otherwise");

    inkwell_stream_close(&stream);
    INKWELL_TEST_FAIL_IF(inkwell_stream_is_open(&stream), "close should close it");
    /* Idempotent: a second close is what a teardown path does after a failed open. */
    inkwell_stream_close(&stream);
    record_success(test_name);
}

INKWELL_TEST_CASE(stream_refuses_everything_while_closed, unit) {
    struct inkwell_stream stream;
    struct inkwell_stream_slot slots[1];
    uint8_t queue[4];
    (void)inkwell_stream_init(&stream, "test", slots, 1U, queue, 4U);

    const uint8_t byte = 1U;
    INKWELL_TEST_FAIL_IF(inkwell_stream_send(&stream, &byte, 1U, 0U) != -ENOTCONN, "send");
    INKWELL_TEST_FAIL_IF(inkwell_stream_flush(&stream) != -ENOTCONN, "flush");
    INKWELL_TEST_FAIL_IF(inkwell_stream_pump(&stream) != -ENOTCONN, "pump");
    INKWELL_TEST_FAIL_IF(inkwell_stream_write_raw(&stream, &byte, 1U) != -ENOTCONN, "write_raw");
    INKWELL_TEST_FAIL_IF(inkwell_stream_send(NULL, &byte, 1U, 0U) != -EINVAL, "a NULL stream");
    INKWELL_TEST_FAIL_IF(inkwell_stream_init(NULL, "x", NULL, 0U, NULL, 0U) != -EINVAL,
                         "init of a NULL stream");
    record_success(test_name);
}
