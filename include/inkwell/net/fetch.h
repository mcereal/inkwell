#pragma once

/*
 * One HTTPS request, made in this process on the event loop.
 *
 * A metadata document, a release asset, a firmware image - whatever an application downloads
 * goes through here. It is net/tls.h under the HTTP/1.1 codec in codec/http.h: resolve, connect,
 * handshake, one request, one reply, and a redirect is the same again against the URL it named.
 * Nothing is forked but the name lookup, which forks without exec (net/resolve.h), so there is no
 * program the device has to have and nothing to install beside the binary - the trust anchors are
 * whatever the application registered with `inkwell_tls_set_roots()`.
 *
 * What this module is not is a state machine. It knows nothing about what is being fetched or
 * what should happen next; it runs one request, and when that is over it says how it went
 * exactly once. Which state that lands in is the caller's: two callers downloading two
 * different things keep two different state machines, and neither is any of this module's
 * business.
 *
 * One request at a time, per fetcher. A caller that needs two documents runs them in sequence,
 * and may start the second from inside the first's completion: the fetcher is fully idle by the
 * time the callback runs, and the body it was handed outlives the call.
 *
 * **https only, and verified.** A plain `http://` URL is refused at start, and so is a redirect
 * to one. The case that makes this non-negotiable rather than a default: a caller that downloads
 * a digest over one connection and the file it checks over another has proved nothing at all if
 * either arrived in the clear.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "inkwell/net/reason.h"
#include "inkwell/net/resolve.h"

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;
struct inkwell_fetch_conn; /* the request in flight; defined in fetch.c */

#define INKWELL_FETCH_PATH_MAX 256U
/* How many headers one request may carry. Two (Accept and User-Agent) covers the common case
   and a range read adds one; the array is fixed so a request is a value with no allocation. */
#define INKWELL_FETCH_HEADERS_MAX 4U
/* One header line, as the caller wrote it. */
#define INKWELL_FETCH_HEADER_MAX 256U
/* A JSON reply that is a few KB, with room for the ones that are a few tens of KB. Anything
   past a request's own cap is a reply we would not trust anyway. */
#define INKWELL_FETCH_RESPONSE_MAX 65536U
/* Hops a request may take before the chain is refused. One is common: a release asset on a large
   forge is typically a 302 from the API host to a CDN. */
#define INKWELL_FETCH_REDIRECTS_MAX 5U

/* How a finished request ended. Every one of these is a sentence the caller has to be able to
   put on a screen, so they are told apart by what the reader would do next. */
enum inkwell_fetch_outcome {
    INKWELL_FETCH_OK = 0,
    /* The server answered, and not with the document: a 404, a 500, a 3xx with nowhere to go.
       `status` is the code. */
    INKWELL_FETCH_HTTP_STATUS,
    INKWELL_FETCH_TOO_LARGE, /* the reply passed the request's cap and was abandoned */
    /* The server could not be reached, or the connection dropped before the reply was whole: a
       name that does not resolve, a refused connect, a body cut short. Worth retrying - and
       which of those it was is `failure` on the result, for a caller that says more than that. */
    INKWELL_FETCH_NETWORK,
    /* The server's certificate did not verify, or the handshake failed. Not worth retrying. */
    INKWELL_FETCH_TLS,
    /* The reply was not HTTP this client will act on: malformed, a redirect off https or past
       INKWELL_FETCH_REDIRECTS_MAX, or a range request answered with the whole file. */
    INKWELL_FETCH_PROTOCOL,
    INKWELL_FETCH_FILE,      /* output_path could not be written */
    INKWELL_FETCH_TIMED_OUT, /* the request's deadline passed */
    INKWELL_FETCH_OUTCOME_COUNT,
};

struct inkwell_fetch_result {
    enum inkwell_fetch_outcome outcome;
    /* The final reply's HTTP status, or 0 when no reply arrived. */
    int status;
    /*
     * The body, always NUL-terminated, or NULL when the request wrote to a file or the reply had
     * none. For a HEAD, the final reply's status line and headers. Valid for the duration of the
     * callback and freed after it, so a caller that wants to keep any of it copies it out.
     */
    const char *body;
    size_t len;
    /* What went wrong, in words, for a log line - a TLS error, an errno, what was wrong with the
       reply. "" on success; never NULL. Valid for the duration of the callback. */
    const char *detail;
    /*
     * Why the connection failed, as a reason and a number (net/reason.h), for NETWORK, TLS and
     * TIMED_OUT; INKWELL_NET_OK for every other outcome, whose problem was the reply rather than
     * the way to it.
     *
     * The outcome says what the reader would do next and this says what happened, and they are
     * kept apart for the reason net/reason.h gives: NETWORK is one thing to do about four things
     * that happened - no such host, a lookup that failed, a refused connect, a peer that hung up
     * - and a caller that wants to say which cannot work it back out of the outcome. A TIMED_OUT
     * request is LOOKUP_TIMED_OUT while the name was still being looked up and TIMED_OUT after.
     */
    struct inkwell_net_failure failure;
    /*
     * The host of the hop the request ended on. Not the subject net/reason.h says a caller
     * already has: after a redirect it is a host the caller never named, and a failure there is
     * a failure somewhere else. Never NULL; valid for the duration of the callback.
     */
    const char *host;
};

/*
 * What a request asks for. GET is the body; HEAD is the headers and nothing else.
 *
 * HEAD exists for one reason and it is not tidiness: a zip is read from the back, the CDN in
 * front of these files answers `501 Unsupported client range` to a suffix range, and so the only
 * way to ask for the last 64 KB of a file is to know how long it is first. The final reply's head
 * is captured as the body, which is what `inkwell_fetch_content_length()` reads.
 */
enum inkwell_fetch_method {
    INKWELL_FETCH_GET = 0,
    INKWELL_FETCH_HEAD,
};

/* Called once per started request, from the event loop, when it is over. */
typedef void (*inkwell_fetch_done_fn)(void *userdata, const struct inkwell_fetch_result *result);

struct inkwell_fetch_request {
    const char *url;
    enum inkwell_fetch_method method;
    /*
     * Whole header lines - "Accept: application/vnd.github+json" - sent on every hop. Entries are
     * read until the first NULL, so a request that sets none leaves the array zeroed. A
     * `User-Agent` is added when none is given, since GitHub's API refuses a request without one.
     *
     * A `Range:` line makes this a range request, and a range request must be answered `206`: a
     * server that ignores the range sends the whole file, and a caller that asked for 64 KB of a
     * 100 MB zip is owed a failure rather than the zip.
     */
    const char *headers[INKWELL_FETCH_HEADERS_MAX];
    /*
     * Where the body goes. NULL captures it in memory and hands it to the callback; a path
     * streams it into that file as it arrives, which is what makes a download's progress a
     * stat() on a file this process named. The file is created only once a 2xx has arrived, so a
     * 404's error page never lands where a download was expected.
     */
    const char *output_path;
    /* The whole request - every lookup, hop and byte - gets this long, or TIMED_OUT. 0 is 30 s. */
    uint32_t timeout_ms;
    /* Cap on a captured reply; 0 takes INKWELL_FETCH_RESPONSE_MAX. Ignored with output_path. */
    size_t response_max;
    /*
     * With output_path: the most bytes the body may write to the file, or 0 for no cap. The 206
     * check above says a server honoured the range, not that it sent only what was asked for, and
     * a reply that keeps going is a file that keeps growing - so a caller that knows how long the
     * answer should be says so, and the first byte past it fails the request TOO_LARGE.
     */
    uint64_t output_max;
    inkwell_fetch_done_fn on_done;
    void *userdata;
};

struct inkwell_fetch {
    struct inkwell_loop *loop;
    struct inkwell_resolve resolve;
    /* The request in flight, or NULL. Heap-held and only while one runs: it carries a reply head
       and a read buffer, which is tens of KB this struct's three owners should not hold idle. */
    struct inkwell_fetch_conn *conn;
    /* The loop's clock as of the last start() or tick(), for the lookups a redirect starts. */
    uint64_t now_ms;
    /* See inkwell_fetch_connect_to(). Empty for the address a URL's host resolves to. */
    char connect_host[192];
    uint16_t connect_port;
    /* The address family the last connection succeeded in, tried first next time; AF_UNSPEC (0)
       until one has. On a network whose IPv6 is broken this is what stops every request paying
       for the discovery. */
    int preferred_family;
};

/*
 * `loop` may be NULL, in which case the fetcher reports itself unavailable - which is what a test
 * that never means to connect anywhere wants. Returns 0, or -errno.
 */
int inkwell_fetch_init(struct inkwell_fetch *fetch, struct inkwell_loop *loop);

/* Abandons anything in flight without reporting it, and releases everything held. */
void inkwell_fetch_shutdown(struct inkwell_fetch *fetch);

/*
 * What a request calls itself: the product token sent as `User-Agent` on every hop of every
 * request that does not carry one of its own - "yourthing/1.4.2". Copied, so a caller may build
 * it on the stack. Set once at startup, from wherever this process knows its own name; process
 * wide rather than per fetcher, because a product does not change its name between downloads.
 *
 * There is a generic default rather than none. A request with no `User-Agent` at all is refused
 * outright by some servers - GitHub's API is one - and forgetting to set this should cost a vague
 * line in somebody else's log, not a download that fails for a reason nothing here would say.
 * NULL or "" puts the default back.
 */
void inkwell_fetch_set_user_agent(const char *product);

/* True when this build has TLS and there is a loop to run a request on. */
bool inkwell_fetch_available(const struct inkwell_fetch *fetch);
/* True while a request is running. A second is refused with -EBUSY. */
bool inkwell_fetch_busy(const struct inkwell_fetch *fetch);

/*
 * Every connection goes to `host`:`port` instead of wherever a URL's host resolves, the way
 * curl's `--resolve` does: `host` is a numeric address, or several comma-separated and tried in
 * order, or a name that is looked up. The URL's host is still what is sent as SNI and `Host`, and
 * what the certificate is checked against, so nothing about verification changes. For tests,
 * which stand a server on loopback and point real URLs at it. NULL puts it back.
 *
 * Without it, a host's addresses are tried in turn: the next on a refusal, or when one has not
 * answered in a few seconds, so one unreachable address - or a whole family - is not the end of
 * the request.
 */
void inkwell_fetch_connect_to(struct inkwell_fetch *fetch, const char *host, uint16_t port);

/*
 * Starts `request`. Returns 0, or -errno: -ENOTSUP when unavailable, -EBUSY with a request
 * already running, -EINVAL for a request with no callback or a URL that is not https, -ENOMEM.
 * On any error nothing was started and `on_done` will not be called.
 *
 * On 0 the callback is called exactly once, later, from the loop - never before this returns.
 */
int inkwell_fetch_start(struct inkwell_fetch *fetch, const struct inkwell_fetch_request *request,
                        uint64_t now_ms);

/*
 * Enforces the deadline, reaps a finished lookup, and resumes a read that gave the loop back
 * early. Call every loop turn.
 */
void inkwell_fetch_tick(struct inkwell_fetch *fetch, uint64_t now_ms);

/* Reads the `Content-Length` out of a captured HEAD reply. False when there is none. */
bool inkwell_fetch_content_length(const char *headers, size_t len, uint64_t *out);

/*
 * Abandons anything in flight: the connection is dropped and the callback is *not* called. For
 * a caller that has decided the outcome itself - a cancelled step, a shutdown - and does not want
 * a completion arriving on top of the decision. A partly written output_path is left for the
 * caller, who named it, to remove.
 */
void inkwell_fetch_cancel(struct inkwell_fetch *fetch);

/* A fixed English name for an outcome, for a log line. Never NULL. */
const char *inkwell_fetch_outcome_name(enum inkwell_fetch_outcome outcome);

#ifdef __cplusplus
}
#endif
