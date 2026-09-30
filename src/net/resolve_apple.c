/*
 * The resolver on a Mac: DNS-SD over the loop, and no child.
 *
 * resolve.c forks and calls getaddrinfo() in the child. That is sound where it was written - a
 * single-threaded process on the Brick, where getaddrinfo() reads files and a socket - and it is
 * not sound here. A Mac process has threads it never started (Cocoa, CoreBluetooth,
 * libdispatch), and after fork() only async-signal-safe calls are allowed in the child.
 * getaddrinfo() on macOS is a round trip to mDNSResponder over XPC, and it faults there.
 *
 * So this asks mDNSResponder directly. DNSServiceGetAddrInfo() hands back a socket, the loop
 * watches it, and DNSServiceProcessResult() runs the callbacks on the loop's thread - which is
 * the whole contract of resolve.h without a process in between.
 *
 * **Both families are asked for, one query each, and the lookup is over when both have
 * answered.** A DNS-SD query does not end on its own: it is a subscription. Asking for each
 * family separately, with kDNSServiceFlagsReturnIntermediates so that a "no" is delivered and
 * not merely implied by silence, is what gives the lookup an end. Letting mDNSResponder pick the
 * families (protocol 0) would not: it never says which ones it picked, so nothing could tell a
 * family that is still coming from one that was never asked.
 *
 * AI_ADDRCONFIG is then applied here, since the query no longer does it: a family this machine
 * has no address in is not offered - see resolve_family_configured().
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "inkwell/net/resolve.h"

#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RESOLVE_V4 = 0, RESOLVE_V6 = 1, RESOLVE_FAMILIES = 2 };

/* Enough answers per family to pick INKWELL_RESOLVE_ADDRESSES_MAX from, alternating. */
#define RESOLVE_ANSWERS_MAX INKWELL_RESOLVE_ADDRESSES_MAX

struct resolve_family {
    DNSServiceRef query;       /* shares the connection below; NULL once it has answered */
    bool heard;                /* something has arrived for it in the batch being delivered */
    bool answered;             /* and that batch is over, so nothing more is coming for it */
    DNSServiceErrorType error; /* the "no", when that is what it answered */
    struct inkwell_resolve_address answers[RESOLVE_ANSWERS_MAX];
    size_t answer_count;
};

struct resolve_apple_backend {
    DNSServiceRef connection;
    int fd;
    uint16_t port;
    bool numeric; /* answered at start(), and handed over by the next tick */
    struct resolve_family families[RESOLVE_FAMILIES];
};

bool inkwell_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                             socklen_t *out_len) {
    return inkwell_socket_parse_literal(host, port, out, out_len);
}

/* ------------------------------------------------------------------ AI_ADDRCONFIG */

static bool resolve_is_loopback(const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)(const void *)address;
        return (ntohl(v4->sin_addr.s_addr) >> 24U) == 127U;
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)(const void *)address;
        return IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) != 0;
    }
    return false;
}

/*
 * Whether this machine has an address of `family` it could reach something else from.
 *
 * getaddrinfo()'s rule, and the reason for it is resolve.c's: a Mac on a network with no IPv6
 * route would otherwise be handed AAAA answers whose connects can only time out. Loopback does
 * not count, and neither does IPv6 link-local, which every interface has whether or not
 * anything routes. An answer that is itself loopback is kept regardless - "localhost" is always
 * reachable - which is resolve_keep()'s half of this.
 */
static bool resolve_family_configured(int family) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return true; /* not knowing is not a reason to throw an answer away */
    }
    bool found = false;
    for (const struct ifaddrs *i = interfaces; i != NULL && !found; i = i->ifa_next) {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != family ||
            (i->ifa_flags & IFF_UP) == 0U || resolve_is_loopback(i->ifa_addr)) {
            continue;
        }
        if (family == AF_INET6) {
            const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)(const void *)i->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) {
                continue;
            }
        }
        found = true;
    }
    freeifaddrs(interfaces);
    return found;
}

static bool resolve_keep(const struct inkwell_resolve_address *answer, const bool configured[2]) {
    const struct sockaddr *address = (const struct sockaddr *)&answer->address;
    return resolve_is_loopback(address) ||
           configured[address->sa_family == AF_INET6 ? RESOLVE_V6 : RESOLVE_V4];
}

/* ------------------------------------------------------------------ the queries */

static void resolve_family_close(struct resolve_family *family) {
    if (family->query != NULL) {
        DNSServiceRefDeallocate(family->query);
        family->query = NULL;
    }
}

/*
 * One answer, or one "no", for one family.
 *
 * Only records it. Whether the lookup is over is decided after DNSServiceProcessResult()
 * returns, so nothing here tears down the connection whose callback this is.
 */
static void DNSSD_API resolve_on_answer(DNSServiceRef query, DNSServiceFlags flags,
                                        uint32_t interface, DNSServiceErrorType error,
                                        const char *host, const struct sockaddr *address,
                                        uint32_t ttl, void *context) {
    (void)interface;
    (void)host;
    (void)ttl;
    struct resolve_apple_backend *backend = (struct resolve_apple_backend *)context;
    struct resolve_family *family = NULL;
    for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
        if (backend->families[i].query == query) {
            family = &backend->families[i];
        }
    }
    if (family == NULL) {
        return; /* a family already closed; nothing it says now is wanted */
    }

    if (error == kDNSServiceErr_NoError && (flags & kDNSServiceFlagsAdd) != 0U && address != NULL &&
        family->answer_count < RESOLVE_ANSWERS_MAX) {
        struct inkwell_resolve_address *answer = &family->answers[family->answer_count];
        memset(answer, 0, sizeof *answer);
        if (address->sa_family == AF_INET) {
            struct sockaddr_in v4;
            memcpy(&v4, address, sizeof v4);
            v4.sin_port = htons(backend->port);
            memcpy(&answer->address, &v4, sizeof v4);
            answer->len = (socklen_t)sizeof v4;
            family->answer_count++;
        } else if (address->sa_family == AF_INET6) {
            struct sockaddr_in6 v6;
            memcpy(&v6, address, sizeof v6);
            v6.sin6_port = htons(backend->port);
            memcpy(&answer->address, &v6, sizeof v6);
            answer->len = (socklen_t)sizeof v6;
            family->answer_count++;
        }
    } else if (error != kDNSServiceErr_NoError) {
        family->error = error;
    }
    family->heard = true;
    /*
     * The end of a batch is the end of an answer: the query is a subscription and would go on
     * reporting changes, which nothing here wants. kDNSServiceFlagsMoreComing is about the
     * *connection*, not this query - with both families' answers in one read, the first is
     * delivered with it set because the second is behind it - so a batch's end finishes every
     * family heard in it, and a "no" is final on its own.
     */
    if (error != kDNSServiceErr_NoError) {
        family->answered = true;
    }
    if ((flags & kDNSServiceFlagsMoreComing) == 0U) {
        for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
            backend->families[i].answered |= backend->families[i].heard;
        }
    }
}

static void resolve_backend_release(struct resolve_apple_backend *backend) {
    if (backend == NULL) {
        return;
    }
    for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
        resolve_family_close(&backend->families[i]);
    }
    if (backend->connection != NULL) {
        DNSServiceRefDeallocate(backend->connection);
    }
    free(backend);
}

static void resolve_release(struct inkwell_resolve *resolve) {
    struct resolve_apple_backend *backend = (struct resolve_apple_backend *)resolve->backend;
    if (backend == NULL) {
        return;
    }
    if (backend->fd >= 0 && resolve->loop != NULL) {
        (void)inkwell_loop_remove_fd(resolve->loop, backend->fd);
    }
    resolve->backend = NULL;
    resolve_backend_release(backend);
}

/* ------------------------------------------------------------------ the outcome */

/*
 * The answers, alternating families from the preferred one (RFC 8305 section 4), for the reason
 * resolve.c's resolve_pick() gives. IPv6 goes first when there is a usable one, which is the
 * order getaddrinfo() would have sorted them into on a network that routes it.
 */
static void resolve_pick(const struct resolve_apple_backend *backend,
                         struct inkwell_resolve_result *result) {
    const bool configured[RESOLVE_FAMILIES] = {resolve_family_configured(AF_INET),
                                               resolve_family_configured(AF_INET6)};
    const struct inkwell_resolve_address *kept[RESOLVE_FAMILIES][RESOLVE_ANSWERS_MAX];
    size_t kept_count[RESOLVE_FAMILIES] = {0U, 0U};
    for (int f = 0; f < RESOLVE_FAMILIES; ++f) {
        const struct resolve_family *family = &backend->families[f];
        for (size_t i = 0U; i < family->answer_count; ++i) {
            if (resolve_keep(&family->answers[i], configured)) {
                kept[f][kept_count[f]++] = &family->answers[i];
            }
        }
    }

    int turn = kept_count[RESOLVE_V6] > 0U ? RESOLVE_V6 : RESOLVE_V4;
    size_t next[RESOLVE_FAMILIES] = {0U, 0U};
    while (
        result->address_count < INKWELL_RESOLVE_ADDRESSES_MAX &&
        (next[RESOLVE_V4] < kept_count[RESOLVE_V4] || next[RESOLVE_V6] < kept_count[RESOLVE_V6])) {
        if (next[turn] >= kept_count[turn]) {
            turn = 1 - turn;
        }
        result->addresses[result->address_count++] = *kept[turn][next[turn]++];
        turn = 1 - turn;
    }
    if (result->address_count > 0U) {
        result->address = result->addresses[0].address;
        result->address_len = result->addresses[0].len;
    }
}

/*
 * With no usable address, the line resolve.c draws between EAI_NONAME and the rest: NOT_FOUND
 * when every family said there is no such name or no such record (or answered only with
 * addresses this machine cannot reach), FAILED when any family's lookup did not work at all -
 * which says nothing about the name.
 */
static enum inkwell_resolve_outcome resolve_outcome_of(const struct resolve_apple_backend *backend,
                                                       int *error) {
    enum inkwell_resolve_outcome outcome = INKWELL_RESOLVE_NOT_FOUND;
    *error = 0;
    for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
        const DNSServiceErrorType why = backend->families[i].error;
        if (why == kDNSServiceErr_NoError) {
            continue;
        }
        if (*error == 0) {
            *error = (int)why;
        }
        if (why != kDNSServiceErr_NoSuchName && why != kDNSServiceErr_NoSuchRecord) {
            *error = (int)why;
            outcome = INKWELL_RESOLVE_FAILED;
        }
    }
    return outcome;
}

/* Hands the outcome over exactly once, with the resolver already idle - see resolve.c. */
static void resolve_complete(struct inkwell_resolve *resolve, bool timed_out) {
    struct resolve_apple_backend *backend = (struct resolve_apple_backend *)resolve->backend;
    if (backend == NULL) {
        return;
    }
    struct inkwell_resolve_result result;
    memset(&result, 0, sizeof result);
    resolve_pick(backend, &result);
    if (result.address_count > 0U) {
        /* A family that has not answered by the deadline is not a reason to throw away the one
           that did: the connect goes to what is known. */
        result.outcome = INKWELL_RESOLVE_OK;
    } else if (timed_out) {
        result.outcome = INKWELL_RESOLVE_TIMED_OUT;
    } else {
        result.outcome = resolve_outcome_of(backend, &result.error);
    }

    const inkwell_resolve_done_fn done = resolve->on_done;
    void *const userdata = resolve->userdata;
    resolve_release(resolve);
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->deadline_ms = 0U;
    if (done != NULL) {
        done(userdata, &result);
    }
}

static int resolve_on_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct inkwell_resolve *resolve = (struct inkwell_resolve *)userdata;
    struct resolve_apple_backend *backend =
        resolve != NULL ? (struct resolve_apple_backend *)resolve->backend : NULL;
    if (backend == NULL) {
        return 0;
    }
    const DNSServiceErrorType processed = DNSServiceProcessResult(backend->connection);
    if (processed != kDNSServiceErr_NoError) {
        /* The daemon went away mid-lookup. Every family still open is a lookup that failed. */
        for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
            if (!backend->families[i].answered) {
                backend->families[i].answered = true;
                backend->families[i].error = processed;
            }
        }
    }
    for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
        if (backend->families[i].answered) {
            resolve_family_close(&backend->families[i]);
        }
    }
    if (backend->families[RESOLVE_V4].answered && backend->families[RESOLVE_V6].answered) {
        resolve_complete(resolve, false);
    }
    return 0;
}

/*
 * The numeric forms inet_pton() refuses and getaddrinfo() takes - `127.1`, `0x7f.1`, a v6 zone -
 * which mDNSResponder would send to DNS as names and get nothing for. AI_NUMERICHOST parses them
 * without looking anything up, and in this process that is safe: it is the child of a fork that
 * may not call getaddrinfo(), not the parent.
 */
static bool resolve_numeric(struct resolve_apple_backend *backend, const char *host,
                            uint16_t port) {
    char service[8];
    (void)snprintf(service, sizeof service, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, service, &hints, &results) != 0) {
        return false;
    }
    for (const struct addrinfo *r = results; r != NULL; r = r->ai_next) {
        struct resolve_family *family =
            &backend->families[r->ai_family == AF_INET6 ? RESOLVE_V6 : RESOLVE_V4];
        if (r->ai_addr != NULL && (size_t)r->ai_addrlen <= sizeof family->answers[0].address &&
            family->answer_count < RESOLVE_ANSWERS_MAX) {
            memcpy(&family->answers[family->answer_count].address, r->ai_addr,
                   (size_t)r->ai_addrlen);
            family->answers[family->answer_count].len = (socklen_t)r->ai_addrlen;
            family->answer_count++;
        }
    }
    freeaddrinfo(results);
    backend->numeric = true;
    return true;
}

/* ------------------------------------------------------------------ api */

int inkwell_resolve_init(struct inkwell_resolve *resolve, struct inkwell_loop *loop) {
    if (resolve == NULL) {
        return -EINVAL;
    }
    memset(resolve, 0, sizeof *resolve);
    resolve->loop = loop;
    resolve->child = -1;
    resolve->child_fd = -1;
    return 0;
}

void inkwell_resolve_shutdown(struct inkwell_resolve *resolve) {
    inkwell_resolve_cancel(resolve);
}

bool inkwell_resolve_available(const struct inkwell_resolve *resolve) {
    return resolve != NULL && resolve->loop != NULL;
}

bool inkwell_resolve_busy(const struct inkwell_resolve *resolve) {
    return resolve != NULL && resolve->backend != NULL;
}

int inkwell_resolve_start(struct inkwell_resolve *resolve, const char *host, uint16_t port,
                          inkwell_resolve_done_fn on_done, void *userdata, uint64_t now_ms) {
    if (resolve == NULL || host == NULL || host[0] == '\0' || on_done == NULL) {
        return -EINVAL;
    }
    if (!inkwell_resolve_available(resolve)) {
        return -ENOTSUP;
    }
    if (resolve->backend != NULL) {
        return -EBUSY;
    }

    struct resolve_apple_backend *backend = calloc(1U, sizeof *backend);
    if (backend == NULL) {
        return -ENOMEM;
    }
    backend->fd = -1;
    backend->port = port;
    if (resolve_numeric(backend, host, port)) {
        resolve->backend = backend;
        resolve->deadline_ms = now_ms;
        resolve->on_done = on_done;
        resolve->userdata = userdata;
        return 0;
    }

    if (DNSServiceCreateConnection(&backend->connection) != kDNSServiceErr_NoError) {
        free(backend);
        return -EIO;
    }
    static const DNSServiceProtocol k_protocol[RESOLVE_FAMILIES] = {kDNSServiceProtocol_IPv4,
                                                                    kDNSServiceProtocol_IPv6};
    for (int i = 0; i < RESOLVE_FAMILIES; ++i) {
        /* A shared query is written through a copy of the connection's ref. */
        backend->families[i].query = backend->connection;
        const DNSServiceErrorType asked = DNSServiceGetAddrInfo(
            &backend->families[i].query,
            kDNSServiceFlagsShareConnection | kDNSServiceFlagsReturnIntermediates |
                kDNSServiceFlagsTimeout,
            kDNSServiceInterfaceIndexAny, k_protocol[i], host, resolve_on_answer, backend);
        if (asked != kDNSServiceErr_NoError) {
            backend->families[i].query = NULL;
            resolve_backend_release(backend);
            return asked == kDNSServiceErr_BadParam ? -EINVAL : -EIO;
        }
    }

    backend->fd = DNSServiceRefSockFD(backend->connection);
    resolve->backend = backend;
    resolve->deadline_ms = now_ms + INKWELL_RESOLVE_TIMEOUT_MS;
    resolve->on_done = on_done;
    resolve->userdata = userdata;
    const int added = backend->fd < 0
                          ? -EIO
                          : inkwell_loop_add_fd(resolve->loop, backend->fd, INKWELL_LOOP_IN,
                                                resolve_on_ready, resolve);
    if (added != 0) {
        backend->fd = -1;
        resolve_release(resolve);
        resolve->on_done = NULL;
        resolve->userdata = NULL;
        return added;
    }
    inkwell_log_debug("resolve", "Looking up %s:%u", host, (unsigned)port);
    return 0;
}

void inkwell_resolve_tick(struct inkwell_resolve *resolve, uint64_t now_ms) {
    if (resolve == NULL || resolve->backend == NULL || now_ms < resolve->deadline_ms) {
        return;
    }
    resolve_complete(resolve, !((struct resolve_apple_backend *)resolve->backend)->numeric);
}

void inkwell_resolve_cancel(struct inkwell_resolve *resolve) {
    if (resolve == NULL) {
        return;
    }
    resolve_release(resolve);
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->deadline_ms = 0U;
}
