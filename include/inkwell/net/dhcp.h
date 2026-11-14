#pragma once

/*
 * One DHCPv4 client: asks for an address, keeps it, and says when it has one and when it lost
 * it (RFC 2131's state machine - selecting, requesting, bound, renewing, rebinding).
 *
 * It owns no socket. A DHCP client's socket is the system's business - bound to one interface,
 * allowed to broadcast with no address of its own, which is a different call on every kernel
 * (io/netif.h opens Linux's) - so the caller sends what this hands it and feeds back what
 * arrives. That keeps the protocol here and the kernel there, and lets a test play the server
 * in-process.
 *
 * Every message is sent broadcast, renewals included: a client renewing may unicast to its server
 * (RFC 2131 4.4.5), but a broadcast reaches it as well and needs no route, and it is what the
 * client must do when rebinding anyway.
 *
 * The times are the caller's: how long to wait for an answer, at first and at most (a retry
 * doubles up to the cap). Applying a lease - the address, the route, the resolver - is the
 * caller's too: `bound` says what the server gave, each time it gives it (a renewal may change
 * the times, or even the name servers), and `lost` says the lease ran out with no answer. After
 * `lost` the client starts again from a DISCOVER by itself.
 */

#include "inkwell/codec/dhcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

struct inkwell_dhcp_lease {
    uint8_t address[4];
    uint8_t prefix;    /* from the subnet mask; 24 when the server sent none */
    uint8_t router[4]; /* 0.0.0.0: none */
    uint8_t dns[INKWELL_DHCP_DNS_MAX][4];
    uint8_t dns_count;
    uint8_t server[4];
    uint32_t lease_s; /* 0xFFFFFFFF: for ever */
};

struct inkwell_dhcp_ops {
    /* Broadcast `len` bytes to the servers. 0, or a negative errno - the client retries on its
       timer either way. Required. */
    int (*send)(void *ctx, const uint8_t *bytes, size_t len);
    /* A lease, first or renewed. Required. */
    void (*bound)(void *ctx, const struct inkwell_dhcp_lease *lease);
    /* The lease ran out (or a server said no to it). Optional. */
    void (*lost)(void *ctx);
};

struct inkwell_dhcp_config {
    uint8_t mac[6];
    const char *hostname; /* sent as option 12; NULL for none */
    uint32_t retry_ms;    /* the first wait for an answer */
    uint32_t retry_max_ms;
};

enum inkwell_dhcp_state {
    INKWELL_DHCP_STOPPED = 0,
    INKWELL_DHCP_SELECTING,
    INKWELL_DHCP_REQUESTING,
    INKWELL_DHCP_BOUND,
    INKWELL_DHCP_RENEWING,
    INKWELL_DHCP_REBINDING,
};

struct inkwell_dhcp_client {
    struct inkwell_loop *loop; /* NULL while stopped */
    int timer;
    enum inkwell_dhcp_state state;
    struct inkwell_dhcp_config config;
    char hostname[INKWELL_DHCP_HOSTNAME_MAX];
    struct inkwell_dhcp_ops ops;
    void *ctx;
    uint32_t xid;
    uint32_t wait_ms;   /* the next retry's */
    unsigned tries;     /* in this state */
    uint64_t started;   /* ms: when this exchange began, for `secs` */
    uint64_t bound_at;  /* ms: when the lease was given */
    uint8_t offered[4]; /* REQUESTING: the address asked for; later, the one held */
    uint8_t server[4];
    struct inkwell_dhcp_lease lease;
    uint32_t renew_s, rebind_s;
    /* While a callback runs, a flag on the caller's stack that stop sets, so the client is not
       touched again once a callback stopped it. Private. */
    int *stopped;
};

/* Starts from a DISCOVER, sent now. 0 or a negative errno: -EINVAL for missing ops or a zero
   retry; -EBUSY when started. A zeroed client is a stopped one. */
int inkwell_dhcp_client_start(struct inkwell_dhcp_client *client, struct inkwell_loop *loop,
                              const struct inkwell_dhcp_config *config,
                              const struct inkwell_dhcp_ops *ops, void *ctx);

/* A datagram that arrived on the client's socket. Anything that is not an answer to this client's
   own transaction is ignored. 0, or -EINVAL for bytes that are not a DHCP message. */
int inkwell_dhcp_client_receive(struct inkwell_dhcp_client *client, const uint8_t *bytes,
                                size_t len);

/* Stops, sending nothing: the lease is left to run out (a RELEASE is a courtesy, and the link
   is usually gone when this is called). Safe on a stopped client and from inside a callback. */
void inkwell_dhcp_client_stop(struct inkwell_dhcp_client *client);

#ifdef __cplusplus
}
#endif
