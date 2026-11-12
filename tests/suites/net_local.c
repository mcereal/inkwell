#define _POSIX_C_SOURCE 200809L

/* A Unix-domain socket at a path, framed: accept with the peer, messages both ways, the stale
   path, and every way a connection ends. */

#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"
#include "inkwell/net/local.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS 8U
#define SLOT_BYTES 64U

/* One end: a connection and the memory it runs in, and what it heard. */
struct end {
    struct inkwell_local_conn conn;
    struct inkwell_stream_slot slots[SLOTS];
    uint8_t bytes[SLOTS * SLOT_BYTES];
    uint8_t frame[32];
    int fds[2];
    bool take_fds; /* open with room for two descriptors, and take one for "attach" */
    int taken;     /* the last descriptor taken, -1 for none */
    char taken_text[16];
    size_t frames;
    size_t last_len;
    char last[32];
    int closed_reason; /* 0 until on_closed */
    bool close_on_frame;
    bool echo;
};

struct fixture {
    struct inkwell_loop loop;
    struct inkwell_local_listener listener;
    char dir[64];
    char path[96];
    struct end server;
    struct end client;
    bool accepted;
    struct inkwell_local_peer peer;
};

static void on_frame(void *userdata, const uint8_t *payload, size_t len) {
    struct end *e = userdata;
    e->frames++;
    e->last_len = len;
    const size_t n = len < sizeof e->last - 1U ? len : sizeof e->last - 1U;
    memcpy(e->last, payload, n);
    e->last[n] = '\0';
    /* Only the message the protocol says carries one: a descriptor may arrive with an earlier
       message's bytes (Linux reads them in one go), never after its own. */
    if (e->take_fds && len == 6U && memcmp(payload, "attach", 6U) == 0) {
        e->taken = inkwell_local_conn_take_fd(&e->conn);
        void *map = NULL;
        if (e->taken >= 0 && inkwell_fd_shared_memory_check(e->taken, 4096U) == 0 &&
            inkwell_fd_map(e->taken, 4096U, false, &map) == 0) {
            memcpy(e->taken_text, map, sizeof e->taken_text - 1U);
            (void)inkwell_fd_unmap(map, 4096U);
        }
    }
    if (e->echo) {
        (void)inkwell_local_conn_send(&e->conn, payload, len);
    }
    if (e->close_on_frame) {
        inkwell_local_conn_close(&e->conn);
    }
}

static void on_closed(void *userdata, int reason) {
    struct end *e = userdata;
    e->closed_reason = reason;
}

static int end_open(struct fixture *f, struct end *e, int fd) {
    const struct inkwell_local_conn_storage storage = {
        .slots = e->slots,
        .slot_count = SLOTS,
        .bytes = e->bytes,
        .slot_bytes = SLOT_BYTES,
        .frame = e->frame,
        .frame_capacity = sizeof e->frame,
        .fds = e->take_fds ? e->fds : NULL,
        .fd_capacity = e->take_fds ? 2U : 0U,
    };
    return inkwell_local_conn_open(&e->conn, fd, &f->loop, &storage, on_frame, on_closed, e);
}

static void on_accept(void *userdata, int fd, const struct inkwell_local_peer *peer) {
    struct fixture *f = userdata;
    if (f->accepted) {
        /* Another listener's liveness probe: the fixture has its one server end already. */
        (void)inkwell_fd_close(fd);
        return;
    }
    f->peer = *peer;
    f->accepted = end_open(f, &f->server, fd) == 0;
    if (!f->accepted) {
        (void)inkwell_fd_close(fd);
    }
}

static void spin(struct fixture *f, bool (*done)(const struct fixture *)) {
    for (int i = 0; i < 200 && !done(f); ++i) {
        (void)inkwell_loop_run(&f->loop, 5);
    }
}

static bool accepted(const struct fixture *f) {
    return f->accepted;
}

/* A listening fixture with a client connected and both ends open; `take_fds` gives the server
   room for descriptors. */
static bool fixture_up_with(struct fixture *f, bool take_fds) {
    memset(f, 0, sizeof *f);
    f->server.take_fds = take_fds;
    f->server.taken = -1;
    snprintf(f->dir, sizeof f->dir, "/tmp/inkwell-local-XXXXXX");
    if (mkdtemp(f->dir) == NULL || inkwell_loop_init(&f->loop) != 0) {
        return false;
    }
    snprintf(f->path, sizeof f->path, "%s/s", f->dir);
    if (inkwell_local_listen(&f->listener, &f->loop, f->path, on_accept, f) != 0) {
        return false;
    }
    const int fd = inkwell_local_connect(f->path);
    if (fd < 0 || end_open(f, &f->client, fd) != 0) {
        return false;
    }
    spin(f, accepted);
    return f->accepted;
}

static bool fixture_up(struct fixture *f) {
    return fixture_up_with(f, false);
}

static void fixture_down(struct fixture *f) {
    inkwell_local_conn_close(&f->client.conn);
    inkwell_local_conn_close(&f->server.conn);
    inkwell_local_listener_close(&f->listener);
    inkwell_loop_shutdown(&f->loop);
    (void)rmdir(f->dir);
}

static bool client_heard_two(const struct fixture *f) {
    return f->client.frames >= 2U;
}

INKWELL_TEST_CASE(local_frames_go_both_ways_and_the_peer_is_known, unit) {
    static struct fixture f;
    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept");
    INKWELL_TEST_FAIL_IF(f.peer.pid != (int64_t)getpid() || f.peer.uid != (int64_t)getuid(),
                         "the peer is this process, as the kernel says");
    f.server.echo = true;
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send(&f.client.conn, (const uint8_t *)"hello", 5U) != 0,
                         "a frame is queued");
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send(&f.client.conn, NULL, 0U) != 0,
                         "an empty frame is a frame");
    spin(&f, client_heard_two);
    INKWELL_TEST_FAIL_IF(f.server.frames != 2U || f.client.frames != 2U,
                         "both frames reach the server and come back");
    INKWELL_TEST_FAIL_IF(f.client.last_len != 0U, "the empty one last, in order");
    static const uint8_t big[SLOT_BYTES];
    INKWELL_TEST_FAIL_IF(
        inkwell_local_conn_send(&f.client.conn, big, SLOT_BYTES - INKWELL_FRAME_HEADER + 1U) !=
            -EMSGSIZE,
        "a payload that leaves no room in its slot for the header is refused");
    INKWELL_TEST_FAIL_IF(
        inkwell_local_conn_send(&f.client.conn, big, SLOT_BYTES - INKWELL_FRAME_HEADER) != 0,
        "one that fits with its header goes");
    fixture_down(&f);
    record_success(test_name);
}

static bool server_closed(const struct fixture *f) {
    return f->server.closed_reason != 0;
}

INKWELL_TEST_CASE(local_a_hang_up_and_an_oversized_frame_end_the_connection, unit) {
    static struct fixture f;
    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept");
    (void)inkwell_local_conn_send(&f.client.conn, (const uint8_t *)"last", 4U);
    (void)inkwell_loop_run(&f.loop, 5);
    inkwell_local_conn_close(&f.client.conn);
    spin(&f, server_closed);
    INKWELL_TEST_FAIL_IF(f.server.frames != 1U || strcmp(f.server.last, "last") != 0,
                         "what was sent before the hang-up is delivered first");
    INKWELL_TEST_FAIL_IF(f.server.closed_reason != -ENOTCONN, "then the hang-up, as ENOTCONN");
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_is_open(&f.server.conn), "and the end is closed");
    fixture_down(&f);

    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept again");
    uint8_t big[40] = {0};
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send(&f.client.conn, big, sizeof big) != 0,
                         "the client may send what the server cannot hold");
    spin(&f, server_closed);
    INKWELL_TEST_FAIL_IF(f.server.closed_reason != -EMSGSIZE || f.server.frames != 0U,
                         "a frame past the server's capacity ends it, undelivered");
    fixture_down(&f);
    record_success(test_name);
}

static bool server_heard_one(const struct fixture *f) {
    return f->server.frames >= 1U;
}

INKWELL_TEST_CASE(local_closing_inside_a_frame_stops_delivery, unit) {
    static struct fixture f;
    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept");
    f.server.close_on_frame = true;
    (void)inkwell_local_conn_send(&f.client.conn, (const uint8_t *)"one", 3U);
    (void)inkwell_local_conn_send(&f.client.conn, (const uint8_t *)"two", 3U);
    spin(&f, server_heard_one);
    for (int i = 0; i < 5; ++i) {
        (void)inkwell_loop_run(&f.loop, 5);
    }
    INKWELL_TEST_FAIL_IF(f.server.frames != 1U, "nothing after the close is delivered");
    INKWELL_TEST_FAIL_IF(f.server.closed_reason != 0, "and an asked-for close is not reported");
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_is_open(&f.server.conn), "it is closed");
    fixture_down(&f);
    record_success(test_name);
}

INKWELL_TEST_CASE(local_a_stale_path_is_cleared_and_a_live_one_is_not, unit) {
    static struct fixture f;
    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept");

    struct inkwell_local_listener second;
    INKWELL_TEST_FAIL_IF(inkwell_local_listen(&second, &f.loop, f.path, on_accept, &f) !=
                             -EADDRINUSE,
                         "a second server on a live path is refused, not let in");

    /* The first server dies without cleaning up: the socket file stays, nobody answers. */
    (void)inkwell_loop_remove_fd(&f.loop, f.listener.fd);
    (void)inkwell_fd_close(f.listener.fd);
    f.listener.fd = -1;
    INKWELL_TEST_FAIL_IF(access(f.path, F_OK) != 0, "the dead server's file is still there");
    INKWELL_TEST_FAIL_IF(inkwell_local_listen(&second, &f.loop, f.path, on_accept, &f) != 0,
                         "a stale socket file is cleared and the path bound");
    inkwell_local_listener_close(&second);
    INKWELL_TEST_FAIL_IF(access(f.path, F_OK) == 0, "closing the listener removes the path");

    /* A regular file at the path is not a socket, and not ours to delete. */
    const int file = open(f.path, O_CREAT | O_WRONLY, 0600);
    (void)close(file);
    INKWELL_TEST_FAIL_IF(inkwell_local_listen(&second, &f.loop, f.path, on_accept, &f) != -EEXIST,
                         "something that is not a socket is refused");
    INKWELL_TEST_FAIL_IF(access(f.path, F_OK) != 0, "and left where it was");
    (void)unlink(f.path);

    INKWELL_TEST_FAIL_IF(inkwell_local_connect(f.path) != -ENOENT, "nothing at the path: ENOENT");
    fixture_down(&f);
    record_success(test_name);
}

static bool server_heard_two(const struct fixture *f) {
    return f->server.frames >= 2U;
}

INKWELL_TEST_CASE(local_descriptors_arrive_with_their_message, unit) {
    static struct fixture f;
    INKWELL_TEST_FAIL_IF(!fixture_up_with(&f, true), "listen, connect and accept");
    const int memory = inkwell_fd_shared_memory(4096U);
    void *map = NULL;
    INKWELL_TEST_FAIL_IF(memory < 0 || inkwell_fd_map(memory, 4096U, true, &map) != 0,
                         "a memory object, mapped");
    memcpy(map, "a buffer", 9U);
    (void)inkwell_fd_unmap(map, 4096U);

    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send(&f.client.conn, (const uint8_t *)"first", 5U) != 0,
                         "a message without one");
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send_fds(&f.client.conn, (const uint8_t *)"attach", 6U,
                                                     &memory, 1U) != 0,
                         "then one with the memory attached");
    spin(&f, server_heard_two);
    INKWELL_TEST_FAIL_IF(f.server.frames != 2U || strcmp(f.server.last, "attach") != 0,
                         "both messages arrive, in order");
    INKWELL_TEST_FAIL_IF(f.server.taken < 0 || strcmp(f.server.taken_text, "a buffer") != 0,
                         "and the descriptor is there to take with the one that carried it");
    (void)close(f.server.taken);
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_take_fd(&f.server.conn) != -ENOENT,
                         "nothing more is waiting");
    INKWELL_TEST_FAIL_IF(inkwell_local_conn_send_fds(&f.client.conn, (const uint8_t *)"x", 1U,
                                                     (const int[]){-1}, 1U) != -EBADF,
                         "a descriptor that is not open is refused");

    /* Three in one message, against room for two: past that, no message can be matched with its
       descriptors, so the connection ends. */
    const int three[3] = {memory, memory, memory};
    f.server.take_fds = false; /* take none, so the room fills */
    INKWELL_TEST_FAIL_IF(
        inkwell_local_conn_send_fds(&f.client.conn, (const uint8_t *)"many", 4U, three, 3U) != 0,
        "the client may send more than the server keeps");
    spin(&f, server_closed);
    INKWELL_TEST_FAIL_IF(f.server.closed_reason != -EMSGSIZE,
                         "more descriptors than there is room for end the connection");
    (void)close(memory);
    fixture_down(&f);

    /* A server that never asked for descriptors does not get them, and is not hurt by them. */
    INKWELL_TEST_FAIL_IF(!fixture_up(&f), "listen, connect and accept again");
    const int again = inkwell_fd_shared_memory(4096U);
    INKWELL_TEST_FAIL_IF(
        inkwell_local_conn_send_fds(&f.client.conn, (const uint8_t *)"one", 3U, &again, 1U) != 0,
        "sent to a server with no room");
    spin(&f, server_heard_one);
    INKWELL_TEST_FAIL_IF(f.server.frames != 1U || f.server.closed_reason != 0,
                         "the message arrives, the descriptor is closed, the connection stays");
    (void)close(again);
    fixture_down(&f);
    record_success(test_name);
}
