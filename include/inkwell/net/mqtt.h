#pragma once

#include "inkwell/net/reason.h"
#include "inkwell/net/resolve.h"
#include "inkwell/net/tls.h"
#include "inkwell/runtime/loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One asynchronous MQTT 3.1.1 connection to a broker.
 *
 * The caller supplies publications and subscription filters and receives matching messages.
 * This client deliberately knows nothing about the application protocol carried in a topic or
 * payload; its business is the MQTT session and the socket underneath it.
 *
 * **Everything is on the one event loop.** The socket is non-blocking, name lookup uses
 * inkwell_resolve's asynchronous backend where available, and TLS is a state machine over the
 * same descriptor. There is no thread here and there is nowhere for one to go.
 *
 * **Nothing is queued across a disconnect.** A publish that arrives while the broker is
 * unreachable is dropped and counted, not held. Replaying stale application data after a
 * reconnect is policy this layer cannot choose. A partial *write* is a different thing and is
 * buffered - that is one packet mid-flight, not a backlog.
 */

/* Fixed capacities keep the client allocation-free and make its memory cost visible. */
#define INKWELL_MQTT_CLIENT_ADDRESS_MAX 256U
#define INKWELL_MQTT_CLIENT_USERNAME_MAX 128U
#define INKWELL_MQTT_CLIENT_PASSWORD_MAX 128U
#define INKWELL_MQTT_CLIENT_CLIENT_ID_MAX 128U

/*
 * How many topic filters one connection may hold, and how long each may be.
 *
 * The fixed table is intentionally modest for embedded callers. Applications needing more can
 * use broader wildcard filters or stand up more than one client.
 */
#define INKWELL_MQTT_CLIENT_FILTERS_MAX 16U
#define INKWELL_MQTT_CLIENT_FILTER_MAX 256U

/*
 * The largest MQTT packet this will assemble or accept.
 *
 * MQTT permits packets far larger than a small client should reserve permanently. A broker
 * message past this capacity is skipped - counted off the stream without being buffered - which
 * keeps the client bounded instead of accepting the 256 MB a remaining length can describe.
 */
#define INKWELL_MQTT_CLIENT_PACKET_MAX 1024U
/* Room for a packet mid-write plus the next one behind it. */
#define INKWELL_MQTT_CLIENT_OUTBOUND_MAX 4096U
#define INKWELL_MQTT_CLIENT_TOPIC_MAX 256U

/* Ports nobody writes down: 1883 is MQTT, 8883 is MQTT over TLS. A target that names its own
   port overrides both. */
#define INKWELL_MQTT_CLIENT_PORT 1883U
#define INKWELL_MQTT_CLIENT_PORT_TLS 8883U

/*
 * How long a quiet connection waits before saying something, and before giving up on the answer.
 *
 * The keepalive is what the broker is told; it disconnects a client that has been silent for
 * one and a half times it. The ping interval is comfortably inside that, and the silence
 * deadline is what notices the other direction - a broker that went away without closing
 * anything, which on a mobile device carried out of WiFi range is the ordinary case rather than the
 * exotic one.
 */
#define INKWELL_MQTT_CLIENT_KEEPALIVE_S 60U
#define INKWELL_MQTT_CLIENT_PING_INTERVAL_MS 30000U
#define INKWELL_MQTT_CLIENT_SILENCE_TIMEOUT_MS 90000U
/* A connect, a TLS handshake and a CONNACK each get their own deadline; none of them is allowed
   to leave the state machine parked forever on a socket that will never answer. */
#define INKWELL_MQTT_CLIENT_CONNECT_TIMEOUT_MS 10000U
#define INKWELL_MQTT_CLIENT_HANDSHAKE_TIMEOUT_MS 15000U

/*
 * Backoff between attempts: doubling from five seconds, capped at five minutes.
 *
 * A broker that is refusing a client is usually doing so for a reason that will still be true
 * in a second, and the far end is somebody else's server. Five minutes keeps retries courteous
 * while still recovering without intervention.
 */
#define INKWELL_MQTT_CLIENT_BACKOFF_BASE_MS 5000U
#define INKWELL_MQTT_CLIENT_BACKOFF_MAX_MS 300000U

/*
 * Where the connection is, for a caller that wants to report progress.
 *
 * One state per distinct sentence, so a status row is a table lookup rather than a chain of
 * conditions over a bag of booleans - and so that "connecting" and "connected but the broker has
 * not accepted us" cannot be confused, which is the difference between a broker that is
 * unreachable and one that is rejecting the password.
 */
enum inkwell_mqtt_client_state {
    INKWELL_MQTT_CLIENT_OFF = 0, /* not asked to run */
    INKWELL_MQTT_CLIENT_RESOLVING,
    INKWELL_MQTT_CLIENT_CONNECTING, /* TCP handshake */
    INKWELL_MQTT_CLIENT_SECURING,   /* TLS handshake */
    INKWELL_MQTT_CLIENT_GREETING,   /* CONNECT is on the wire, CONNACK has not come back */
    INKWELL_MQTT_CLIENT_READY,      /* the broker accepted us */
    INKWELL_MQTT_CLIENT_WAITING,    /* an attempt failed; the next one is scheduled */
    INKWELL_MQTT_CLIENT_STATE_COUNT,
};

/*
 * The failures this proxy names itself, as opposed to the ones any link has.
 *
 * Getting to a host is `enum inkwell_net_reason` and is inkwell's, because every link that
 * reaches a network fails in the same ways. What is left over is this: the broker's own answer
 * to being asked for a session, and the two this refuses before it tries. Neither set carries a
 * word; the caller maps the record to whatever user-facing text its application needs.
 *
 * The refusals are kept apart rather than collapsed into "the broker said no" because they are
 * three different things to do next: a wrong password is a setting, a client that is not
 * permitted is an account, and a broker that is unavailable is somebody else's outage.
 */
enum inkwell_mqtt_refusal {
    INKWELL_MQTT_REFUSAL_NONE = 0,
    /* Refused before anything was attempted: `config.address` is not a host and a port, or TLS
       was asked for and this build has none. Both are a setting to change rather than a
       network to wait on, which is why they are not INKWELL_NET_BAD_ADDRESS and a timeout. */
    INKWELL_MQTT_REFUSAL_BAD_ADDRESS,
    INKWELL_MQTT_REFUSAL_NO_TLS,
    /* Something answered and it is not speaking MQTT: a malformed packet, a packet out of
       order, or a length this could not make sense of. */
    INKWELL_MQTT_REFUSAL_PROTOCOL,
    /* The three CONNACK codes worth their own sentence. */
    INKWELL_MQTT_REFUSAL_BAD_LOGIN,
    INKWELL_MQTT_REFUSAL_NOT_ALLOWED,
    INKWELL_MQTT_REFUSAL_BROKER_BUSY,
    /* Any other CONNACK refusal; `code` carries the number, which is all anyone could act on. */
    INKWELL_MQTT_REFUSAL_OTHER,
    INKWELL_MQTT_REFUSAL_COUNT,
};

/*
 * One failure, waiting to be shown. Exactly one half is set: `net.reason` is INKWELL_NET_OK
 * when the broker or this proxy refused, and `refusal` is NONE when the network did.
 */
struct inkwell_mqtt_client_failure {
    struct inkwell_net_failure net;
    enum inkwell_mqtt_refusal refusal;
    uint8_t code; /* the CONNACK code, for INKWELL_MQTT_REFUSAL_OTHER */
};

/* What to connect to. Copied on start(), so the caller's buffer need not outlive the call. */
struct inkwell_mqtt_client_config {
    /* "host", "host:port", or "[v6]:port". An empty address is refused. */
    char address[INKWELL_MQTT_CLIENT_ADDRESS_MAX];
    char username[INKWELL_MQTT_CLIENT_USERNAME_MAX];
    char password[INKWELL_MQTT_CLIENT_PASSWORD_MAX];
    /*
     * Must be unique on the broker. Two clients presenting the same id is not an error a broker
     * reports - it disconnects the older one - so two clients with the same id take turns
     * kicking each other off and look like a broker that flaps.
     */
    char client_id[INKWELL_MQTT_CLIENT_CLIENT_ID_MAX];
    bool tls_enabled;
};

/*
 * A message from the broker, on a topic this connection subscribed to. `topic` is
 * NUL-terminated and `payload` is not; both are only valid for the duration of the call.
 *
 * Called from the event loop, never from publish() or start().
 */
typedef void (*inkwell_mqtt_client_message_fn)(void *userdata, const char *topic,
                                               const uint8_t *payload, size_t len);

/* Told once per transition, for a caller that wants to react rather than poll. May be NULL. */
typedef void (*inkwell_mqtt_client_state_fn)(void *userdata, enum inkwell_mqtt_client_state state);

/*
 * What has gone through this client. Counted for the life of the client
 * rather than of one connection - `connections` is what makes a link that keeps dropping and
 * remaking itself visible, and it could not do that if a reconnection cleared the rest.
 */
struct inkwell_mqtt_client_stats {
    uint32_t published;   /* publishes handed to the socket */
    uint32_t received;    /* messages delivered to the callback */
    uint32_t dropped;     /* publishes refused: not connected, too large, or no room */
    uint32_t skipped;     /* inbound messages too large to forward, counted off the stream */
    uint32_t connections; /* successful CONNACKs, so a flapping link is visible as one */
};

struct inkwell_mqtt_client {
    struct inkwell_loop *loop;
    enum inkwell_mqtt_client_state state;
    struct inkwell_mqtt_client_config config;

    /* The host and port taken out of config.address once, at start. */
    char host[INKWELL_MQTT_CLIENT_ADDRESS_MAX];
    uint16_t port;

    struct inkwell_resolve resolve;
    int fd;
    bool fd_registered;
    bool want_write; /* INKWELL_LOOP_OUT is armed because something is waiting to go out */

    /* Changes whenever start(), stop(), or shutdown() replaces the active session. Packet
       handlers use it to notice that a callback replaced the connection underneath them. */
    uint64_t generation;

    /* Only started when config.tls_enabled; a plaintext connection never touches it, and
       `tls.state` being non-NULL is what every read and write branches on. */
    struct inkwell_tls_client tls;
    /* Empty for the roots registered with inkwell_tls_set_roots(). */
    char ca_bundle[256];

    uint8_t in[INKWELL_MQTT_CLIENT_PACKET_MAX];
    size_t in_len;
    /*
     * The socket was still readable when the per-turn read budget ran out - or, under TLS,
     * plaintext is already decrypted and sitting inside the session where the loop cannot see it
     * and will never report it again. Either way the next tick() has to come back and read
     * rather than wait to be woken.
     */
    bool more_to_read;
    /*
     * A TLS read blocked on *writability* rather than on more bytes arriving.
     *
     * The two directions come apart under TLS: `mbedtls_ssl_read()` may have to send something
     * before it can return anything - refusing a renegotiation with an alert is the reachable
     * case, since this client never enables renegotiation and a TLS 1.2 broker is free to ask -
     * and on a full socket that send is what reports WANT_WRITE. Waiting only for
     * INKWELL_LOOP_IN would wait for the wrong event: the peer is not going to speak again until
     * we have spoken.
     *
     * Kept apart from `tls.wants_write`, which belongs to whichever call blocked last and is
     * cleared by the next one that succeeds. A write finishing must not retract a read's claim
     * on INKWELL_LOOP_OUT.
     */
    bool read_wants_write;
    /* Bytes of an oversized inbound body still to be read and discarded. The stream stays in
       sync because these are counted off it; nothing is ever re-synchronised by guessing. */
    size_t skip_remaining;

    uint8_t out[INKWELL_MQTT_CLIENT_OUTBOUND_MAX];
    size_t out_len;
    size_t out_sent; /* cursor into out[], for a write the socket only took part of */
    /*
     * The length handed to a write that could not finish, to be retried verbatim.
     *
     * A TLS requirement rather than a socket one: `mbedtls_ssl_write()` that returns WANT_WRITE
     * has committed to a record of a particular length and must be called again with the same
     * arguments. Coming back with a longer buffer - because a publish was queued in between -
     * is undefined behaviour in the library.
     */
    size_t out_pending;

    char filters[INKWELL_MQTT_CLIENT_FILTERS_MAX][INKWELL_MQTT_CLIENT_FILTER_MAX];
    size_t filter_count;
    /* Which filter is waiting for its SUBACK, and under what id. Subscriptions go out one at a
       time so a refusal can be attributed to the filter that caused it. */
    size_t filter_sent;
    uint16_t subscribe_id;

    /*
     * The clock, set by tick() and read by everything else.
     *
     * Half of what this module times is *set* from an fd callback and *compared* in tick(): a
     * deadline armed when a socket connects, a retry armed when one fails. Reading the real
     * clock at each of those points would be two different numbers, and would leave a caller
     * driving a synthetic clock - every test here - comparing its own time against the
     * machine's. One source, slightly stale inside a callback, monotonic either way.
     */
    uint64_t now_ms;
    uint64_t deadline_ms; /* the current state's, or 0 when it has none */
    uint64_t next_ping_ms;
    uint64_t last_heard_ms;
    uint64_t retry_at_ms;
    uint32_t failures; /* consecutive, for the backoff; cleared by a CONNACK */

    struct inkwell_mqtt_client_stats stats;
    /* Why the last attempt failed. See inkwell_mqtt_client_failure(). */
    struct inkwell_mqtt_client_failure failure;

    inkwell_mqtt_client_message_fn on_message;
    inkwell_mqtt_client_state_fn on_state;
    void *userdata;
};

/*
 * Prepares a proxy that is not running. `loop` may be NULL, which leaves it permanently OFF -
 * what a caller that has no loop, and a test that means to spawn nothing, both want.
 * Returns 0, or -EINVAL.
 */
int inkwell_mqtt_client_init(struct inkwell_mqtt_client *proxy, struct inkwell_loop *loop);

/* Drops any connection without reporting it and releases everything held. */
void inkwell_mqtt_client_shutdown(struct inkwell_mqtt_client *proxy);

/*
 * A CA bundle file to verify against in place of the registered roots, or NULL to use those
 * roots. Ignored without TLS.
 */
void inkwell_mqtt_client_set_ca_bundle(struct inkwell_mqtt_client *proxy, const char *path);

/*
 * Points the proxy at a broker and starts trying to reach it.
 *
 * A successful call replaces whatever it was doing, including an established connection: a
 * changed address is a different broker. A refused one changes nothing - the previous connection
 * is left alone rather than torn down over a configuration that could not be used anyway.
 *
 * `on_message` may be NULL, which subscribes to nothing usefully but is legal. Returns 0, or
 * -ENOTSUP with no loop, -EINVAL for an address that is not a host or a missing client id. On an
 * error nothing was opened.
 *
 * Returning 0 means the attempt is under way, not that it worked; watch the state.
 */
int inkwell_mqtt_client_start(struct inkwell_mqtt_client *proxy,
                              const struct inkwell_mqtt_client_config *config,
                              inkwell_mqtt_client_message_fn on_message,
                              inkwell_mqtt_client_state_fn on_state, void *userdata,
                              uint64_t now_ms);

/*
 * Disconnects and goes to OFF. A connected proxy sends a DISCONNECT first, which is what tells
 * the broker this was deliberate - without it a client that goes quiet looks to the broker like
 * one that crashed, and a broker with a will message set would act on it.
 */
void inkwell_mqtt_client_stop(struct inkwell_mqtt_client *proxy);

/*
 * Adds a topic filter. Subscribed immediately when the connection is up, and on every
 * reconnection afterwards - a broker with a clean session remembers nothing, so the set has to
 * be re-sent each time rather than assumed.
 *
 * Returns 0, -EEXIST when the filter is already held (which is not a failure and is how a caller
 * that re-derives the set every config change stays idempotent), -ENOSPC when the table is full,
 * or -EINVAL for a filter that is empty or too long.
 */
int inkwell_mqtt_client_subscribe(struct inkwell_mqtt_client *proxy, const char *filter);

/* Empties the filter table. Does not unsubscribe on the wire: the only caller is about to
   re-derive the whole set, and the connection is dropped and remade around it. */
void inkwell_mqtt_client_clear_filters(struct inkwell_mqtt_client *proxy);

/*
 * Puts one message on the broker, at QoS 0.
 *
 * Returns 0, or -ENOTCONN when the broker has not accepted us - which is the ordinary answer
 * while a link is down and is counted, not logged. Also
 * -EINVAL for a topic that is empty or carries a wildcard, -EMSGSIZE for a message larger than
 * this will assemble, and -ENOSPC when the outbound buffer has not drained. Every one of those
 * increments `dropped`.
 */
int inkwell_mqtt_client_publish(struct inkwell_mqtt_client *proxy, const char *topic,
                                const uint8_t *payload, size_t len, bool retained);

/*
 * Drives the deadlines, the keepalive and the backoff. Call every loop turn.
 *
 * The fd callback does the reading and writing; this does everything that is a clock rather than
 * a descriptor, including the retry that has no descriptor to be woken by.
 */
void inkwell_mqtt_client_tick(struct inkwell_mqtt_client *proxy, uint64_t now_ms);

enum inkwell_mqtt_client_state inkwell_mqtt_client_state(const struct inkwell_mqtt_client *proxy);
/* True only in READY: the one state in which a publish will be taken. */
bool inkwell_mqtt_client_is_ready(const struct inkwell_mqtt_client *proxy);
struct inkwell_mqtt_client_stats inkwell_mqtt_client_stats(const struct inkwell_mqtt_client *proxy);
/*
 * Why the last attempt failed. Zeroed - net.reason INKWELL_NET_OK, refusal NONE - until one
 * does, and cleared by the next start.
 *
 * A record rather than a sentence, because this file has no business choosing words.
 */
struct inkwell_mqtt_client_failure
inkwell_mqtt_client_failure(const struct inkwell_mqtt_client *proxy);
/*
 * The TLS library's own account of a handshake that failed, or "" when there is none.
 *
 * Not translated and not a catalog entry, in the same category as the C library's word for an
 * errno: it is a diagnostic, and it is here because a reason and a number cannot reconstruct
 * it - Mbed TLS's message is the only description of that failure there is.
 */
const char *inkwell_mqtt_client_tls_error(const struct inkwell_mqtt_client *proxy);
/*
 * A failure's reason as a short ASCII name - "unreachable", "bad-login" - for a log line.
 * Never NULL, and "none" when nothing has failed.
 *
 * One call rather than two, because the record has two halves and exactly one is set: reading
 * `net.reason` on a refusal gives "ok", which is both wrong and the most convincing kind of
 * wrong, since it is a real name for a real value. Whoever writes a log line should not have to
 * remember which half to look at.
 *
 * Not the exception to this file dealing in no words: inkwell's log is deliberately
 * untranslated, and these are symbol names in the same class as strerror() output. Nothing here
 * is fit to put on a screen.
 */
const char *inkwell_mqtt_client_failure_name(const struct inkwell_mqtt_client_failure *failure);
/* The broker being talked to, for a status row. Never NULL; "" before a start. */
const char *inkwell_mqtt_client_host(const struct inkwell_mqtt_client *proxy);
/* The address as it was configured, which is what a bad one has to be shown as: it never
   became a host. Never NULL; "" before a start. */
const char *inkwell_mqtt_client_address(const struct inkwell_mqtt_client *proxy);

#ifdef __cplusplus
}
#endif
