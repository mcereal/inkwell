#pragma once

/*
 * One non-blocking TCP connection, from a host and port to an owned descriptor.
 *
 * This is the small piece every TCP client otherwise writes again: recognise a numeric address
 * or resolve a name without blocking the loop, open the socket, watch a non-blocking connect,
 * and stop waiting at a caller-selected deadline. It does not own a protocol or a reconnect
 * policy. On success it hands one connected descriptor to the callback, which commonly passes
 * it straight to `inkwell_stream_open()`.
 */

#include "inkwell/net/reason.h"
#include "inkwell/net/resolve.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

enum inkwell_tcp_connect_state {
    INKWELL_TCP_CONNECT_IDLE = 0,
    INKWELL_TCP_CONNECT_RESOLVING,
    INKWELL_TCP_CONNECT_CONNECTING,
};

/* Socket policy applied before connect(). Zero keepalive tunables leave the platform defaults
   alone; this lets a caller enable SO_KEEPALIVE without assuming Linux's TCP_KEEP* vocabulary. */
struct inkwell_tcp_connect_options {
    uint64_t timeout_ms;
    bool no_delay;
    bool keepalive;
    unsigned keepalive_idle_s;
    unsigned keepalive_interval_s;
    unsigned keepalive_count;
};

struct inkwell_tcp_connect_result {
    /* On success, a connected non-blocking descriptor now owned by the callback. -1 on failure. */
    int fd;
    struct inkwell_net_failure failure;
};

typedef void (*inkwell_tcp_connect_done_fn)(void *userdata,
                                            const struct inkwell_tcp_connect_result *result);

struct inkwell_tcp_connector {
    struct inkwell_loop *loop;
    struct inkwell_resolve resolve;
    enum inkwell_tcp_connect_state state;
    int fd;
    bool fd_registered;
    uint64_t deadline_ms;
    uint64_t now_ms;
    struct inkwell_tcp_connect_options options;
    inkwell_tcp_connect_done_fn on_done;
    void *userdata;
};

/* `loop` may be NULL. Numeric addresses can still complete synchronously; a name or a connect
   that would block is refused with -ENOTSUP because there is nowhere to watch it. */
int inkwell_tcp_connector_init(struct inkwell_tcp_connector *connector, struct inkwell_loop *loop);
void inkwell_tcp_connector_shutdown(struct inkwell_tcp_connector *connector);
bool inkwell_tcp_connector_busy(const struct inkwell_tcp_connector *connector);

/*
 * Begins one attempt. `host` is a host only (no brackets or port); `port` is already parsed.
 * `options`, the callback and userdata are copied/borrowed until completion.
 *
 * Returns 0 once the attempt is accepted, or -errno before any callback is owed. When `failure`
 * is non-NULL it is cleared first and filled for a synchronous refusal. An accepted attempt
 * calls `on_done` exactly once unless cancelled. The callback may run before this returns when a
 * numeric loopback connect completes immediately.
 */
int inkwell_tcp_connector_start(struct inkwell_tcp_connector *connector, const char *host,
                                uint16_t port, const struct inkwell_tcp_connect_options *options,
                                inkwell_tcp_connect_done_fn on_done, void *userdata,
                                uint64_t now_ms, struct inkwell_net_failure *failure);

/* Drives the resolver and the connect deadline. Call once per loop turn. */
void inkwell_tcp_connector_tick(struct inkwell_tcp_connector *connector, uint64_t now_ms);

/* Abandons an attempt without calling its completion. Safe while idle. */
void inkwell_tcp_connector_cancel(struct inkwell_tcp_connector *connector);

#ifdef __cplusplus
}
#endif
