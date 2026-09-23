#pragma once

/*
 * One hostname turned into a socket address without blocking the caller's event loop.
 *
 * `getaddrinfo()` is the one POSIX call a single-threaded process cannot make on its own thread:
 * it blocks for as long as the network takes to answer, there is no non-blocking form, and
 * `getaddrinfo_a()` starts threads - so a name typed into a field would be seconds of frozen UI.
 * That is why a TCP link tends to take a numeric address and nothing else for as long as it can
 * get away with.
 *
 * On POSIX the way out is to fork, let the child block, and read the answer back through the
 * event loop. Windows currently supports numeric literals only; hostname lookup refuses with
 * -ENOTSUP until a loop-integrated asynchronous resolver is available there.
 * The child does not exec. There is nothing to exec - `getent` is not on the Brick and busybox's
 * `nslookup` prints a different thing every version - and the resolver we want is the one this
 * binary is already linked against.
 *
 * **A literal is not a lookup.** inkwell_resolve_literal() answers `192.168.1.50` and `fd00::1`
 * with inet_pton and no child at all, which is both faster and what keeps the behaviour a caller
 * already had for an address exactly as it was. A caller checks that first and only starts a
 * lookup for what is left; on POSIX start() therefore always forks and always reports later,
 * and never calls back before it returns.
 *
 * One lookup at a time. A second is refused with -EBUSY, which is all either caller needs: a link
 * resolves one host because it is about to connect to one host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* How long a lookup is given before the child is killed and the outcome is TIMED_OUT. A DNS
   server that is there answers in milliseconds; one that is not is what this is for. */
#define INKWELL_RESOLVE_TIMEOUT_MS 5000U
/* How many of getaddrinfo()'s answers come back. See inkwell_resolve_result.addresses. */
#define INKWELL_RESOLVE_ADDRESSES_MAX 4U

struct inkwell_resolve_address {
    struct sockaddr_storage address; /* the port already set */
    socklen_t len;
};

/* How a finished lookup ended. Each of these is a different sentence on a screen, which is why
   "there is no such name" is not folded in with "the resolver did not work". */
enum inkwell_resolve_outcome {
    INKWELL_RESOLVE_OK = 0,
    /* The name does not resolve: NXDOMAIN, or a name with no address record. The user typed
       something wrong, or the host is not on this network. */
    INKWELL_RESOLVE_NOT_FOUND,
    /* The lookup itself did not work - no resolver configured, no route to the DNS server, the
       child could not be read. Says nothing about whether the name exists. */
    INKWELL_RESOLVE_FAILED,
    INKWELL_RESOLVE_TIMED_OUT,
    INKWELL_RESOLVE_OUTCOME_COUNT,
};

struct inkwell_resolve_result {
    enum inkwell_resolve_outcome outcome;
    /*
     * The `EAI_*` code the child got, or 0. Worth logging through gai_strerror() and not worth
     * showing: "Temporary failure in name resolution" is not a sentence that helps somebody
     * holding a handheld, which is what the outcome above is for.
     */
    int error;
    /*
     * The address, with the port already set, valid only when the outcome is OK.
     *
     * **The first one `getaddrinfo()` returned.** A link connects to this and nothing else: a
     * host on the local network or a broker is not a case where walking a second A record is
     * what fixes a failed connect - a retry goes back through the
     * whole attempt, name included, which is also how it picks up a DHCP lease that moved.
     */
    struct sockaddr_storage address;
    socklen_t address_len;
    /*
     * The first few answers, `address` among them, **alternating families** starting with the
     * first answer's (RFC 8305 section 4). For an HTTP client, which tries the next when one will
     * not connect: a CDN host has several addresses, and a network with an IPv6 address and no IPv6
     * route answers every AAAA first. Alternating is what keeps a broken family from being
     * every address on the list.
     */
    struct inkwell_resolve_address addresses[INKWELL_RESOLVE_ADDRESSES_MAX];
    size_t address_count;
};

/* Called once per started lookup, from the event loop, when the child is gone. */
typedef void (*inkwell_resolve_done_fn)(void *userdata,
                                        const struct inkwell_resolve_result *result);

struct inkwell_resolve {
    struct inkwell_loop *loop;

    /* The running child, or -1. Only ever one. */
    pid_t child;
    int child_fd;
    uint64_t deadline_ms;

    /* The child writes one fixed-size record; this is how much of it has arrived. */
    uint8_t record[(sizeof(struct sockaddr_storage) + 8U) * INKWELL_RESOLVE_ADDRESSES_MAX + 8U];
    size_t record_len;
    /* Set by a read that gave up, so the reap reports why rather than the exit status. */
    enum inkwell_resolve_outcome failure;

    inkwell_resolve_done_fn on_done;
    void *userdata;
};

/*
 * Fills `out` from a numeric host - v4 or v6, no brackets - and sets the port.
 *
 * True when `host` was a literal and nothing needs looking up. False means it is a name, which
 * is the caller's cue to start a lookup; it is not an error.
 */
bool inkwell_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                             socklen_t *out_len);

/* `loop` may be NULL, in which case the resolver reports itself unavailable - which is what a
   test that never means to fork anything wants. Returns 0, or -errno. */
int inkwell_resolve_init(struct inkwell_resolve *resolve, struct inkwell_loop *loop);

/* Kills anything in flight without reporting it, and releases everything held. */
void inkwell_resolve_shutdown(struct inkwell_resolve *resolve);

/* True when asynchronous hostname lookup is supported and a loop is supplied. */
bool inkwell_resolve_available(const struct inkwell_resolve *resolve);
/* True while a lookup is running. A second is refused with -EBUSY. */
bool inkwell_resolve_busy(const struct inkwell_resolve *resolve);

/*
 * Starts looking `host` up, with `port` written into whatever comes back.
 *
 * Returns 0, or -errno: -ENOTSUP without an available asynchronous resolver (currently all of
 * Windows), -EBUSY with a lookup already running, -EINVAL for an empty name or no callback. On
 * any error no work was started and `on_done` will not be called.
 *
 * On 0 the callback is called exactly once, later, from the loop. It is never called before this
 * returns, including for a name that turns out to be a literal - see inkwell_resolve_literal().
 */
int inkwell_resolve_start(struct inkwell_resolve *resolve, const char *host, uint16_t port,
                          inkwell_resolve_done_fn on_done, void *userdata, uint64_t now_ms);

/*
 * Enforces the deadline and reaps a finished child. Call every loop turn.
 *
 * Both halves matter: the fd callback sees EOF, but a
 * child that wrote its answer and has not yet been reaped is only ever finished here.
 */
void inkwell_resolve_tick(struct inkwell_resolve *resolve, uint64_t now_ms);

/*
 * Abandons anything in flight: the child is killed and the callback is *not* called. For a
 * caller that has decided the outcome itself - a link torn down while a name was being looked
 * up - and does not want a completion arriving on top of the decision.
 */
void inkwell_resolve_cancel(struct inkwell_resolve *resolve);

#ifdef __cplusplus
}
#endif
