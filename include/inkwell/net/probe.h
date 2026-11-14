#pragma once

/*
 * One plain-HTTP request whose answer is the point, not its body: the status and where it says
 * to go next, from a server nobody has to trust.
 *
 * net/fetch.h refuses `http://` outright, and it is right to: a document that arrived in the
 * clear proves nothing. A probe is the one request where the clear is what is being asked
 * about. A network that intercepts traffic until someone signs in - a hotel's, a train's, a
 * cafe's - shows itself only on a request it can intercept, so the question "does a plain GET
 * of a page known to answer 204 come back 204?" has to be asked over plain HTTP. If it comes
 * back 200 with a login form, or a 302 to one, the network is in the way.
 *
 * So this is narrower than a fetch in every direction that matters:
 *
 *  - **http only.** An `https://` URL is fetch's, and refused here.
 *  - **Never followed.** A redirect is the answer: its status and its `Location`, resolved
 *    against the URL, are handed back, and nothing is requested from where it points.
 *  - **The head only.** The request ends once the status line and headers are in; no body is
 *    read, kept or handed back, so nothing a hostile network sends ends up anywhere.
 *  - **One at a time**, like every client here.
 *
 * Underneath it is net/tcp.h (the lookup and the connect) and codec/http.h (the bytes).
 */

#include "inkwell/base/fd.h"
#include "inkwell/net/reason.h"
#include "inkwell/net/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;
struct inkwell_probe_conn; /* the request in flight; defined in probe.c */

/* A `Location` worth keeping: a sign-in page's address. A longer one is handed back as "". */
#define INKWELL_PROBE_LOCATION_MAX 512U
/* The request's whole deadline when the caller gives 0. */
#define INKWELL_PROBE_TIMEOUT_MS 10000U

enum inkwell_probe_outcome {
    /* A server answered: `status`, and `location` for a redirect. */
    INKWELL_PROBE_ANSWERED = 0,
    /* No answer: the name, the connect, or a connection closed before a whole head. `failure`
       says which. */
    INKWELL_PROBE_NETWORK,
    /* An answer that is not HTTP: a status line or header codec/http.h refuses. */
    INKWELL_PROBE_PROTOCOL,
    INKWELL_PROBE_TIMED_OUT,
    INKWELL_PROBE_OUTCOME_COUNT,
};

struct inkwell_probe_result {
    enum inkwell_probe_outcome outcome;
    int status; /* ANSWERED: the HTTP status; 0 otherwise */
    /* ANSWERED with a `Location`: it, made absolute against the URL probed. "" for none, or for
       one that is not a URL or does not fit. Never NULL; valid for the call. */
    const char *location;
    struct inkwell_net_failure failure; /* NETWORK and TIMED_OUT */
};

typedef void (*inkwell_probe_done_fn)(void *userdata, const struct inkwell_probe_result *result);

struct inkwell_probe {
    struct inkwell_loop *loop;
    struct inkwell_tcp_connector connector;
    /* The request in flight, or NULL: heap-held only while one runs, since it carries a head. */
    struct inkwell_probe_conn *conn;
};

/* `loop` may be NULL: then every start is refused with -ENOTSUP. 0, or -errno. */
int inkwell_probe_init(struct inkwell_probe *probe, struct inkwell_loop *loop);
/* Abandons anything in flight without reporting it. */
void inkwell_probe_shutdown(struct inkwell_probe *probe);
bool inkwell_probe_busy(const struct inkwell_probe *probe);

/*
 * GETs `url`, which must be `http://`. 0 once started, and `done` is then called exactly once,
 * from the loop or a tick, never before this returns; or -errno with nothing started: -EINVAL
 * for a URL that is not plain http or no callback, -EBUSY with one running, -ENOTSUP without a
 * loop, -ENOMEM. `timeout_ms` 0 is INKWELL_PROBE_TIMEOUT_MS.
 */
int inkwell_probe_start(struct inkwell_probe *probe, const char *url, uint32_t timeout_ms,
                        inkwell_probe_done_fn done, void *userdata, uint64_t now_ms);

/* Drives the lookup and the deadline. Call every loop turn while busy. */
void inkwell_probe_tick(struct inkwell_probe *probe, uint64_t now_ms);

/* Abandons the request in flight; its callback is not called. Safe while idle. */
void inkwell_probe_cancel(struct inkwell_probe *probe);

/* A fixed English name for an outcome, for a log line. Never NULL. */
const char *inkwell_probe_outcome_name(enum inkwell_probe_outcome outcome);

#ifdef __cplusplus
}
#endif
