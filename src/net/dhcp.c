#include "inkwell/net/dhcp.h"

#include "inkwell/base/random.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define REQUEST_TRIES 4U    /* a REQUEST unanswered this often: start again from a DISCOVER */
#define ARM_MAX_MS 3600000U /* a long lease is waited out an hour at a time */
#define FOREVER 0xFFFFFFFFU

static int zero4(const uint8_t a[4]) {
    return a[0] == 0U && a[1] == 0U && a[2] == 0U && a[3] == 0U;
}

static uint32_t new_xid(void) {
    uint32_t xid = 0U;
    if (inkwell_random_bytes(&xid, sizeof xid) != 0) {
        xid = (uint32_t)inkwell_time_monotonic_ms() * 2654435761U;
    }
    return xid;
}

static void arm(struct inkwell_dhcp_client *c, uint64_t ms) {
    if (ms == 0U) {
        ms = 1U;
    }
    (void)inkwell_timer_arm_once(c->timer, (uint32_t)(ms > ARM_MAX_MS ? ARM_MAX_MS : ms));
}

/* The wait before the next retry, doubling up to the cap. */
static uint32_t next_wait(struct inkwell_dhcp_client *c) {
    const uint32_t wait = c->wait_ms;
    c->wait_ms =
        c->wait_ms >= c->config.retry_max_ms / 2U ? c->config.retry_max_ms : c->wait_ms * 2U;
    return wait;
}

static void send_message(struct inkwell_dhcp_client *c, enum inkwell_dhcp_type type) {
    struct inkwell_dhcp_message m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.xid = c->xid;
    const uint64_t secs = (inkwell_time_monotonic_ms() - c->started) / 1000U;
    m.secs = (uint16_t)(secs > 0xFFFFU ? 0xFFFFU : secs);
    memcpy(m.mac, c->config.mac, sizeof m.mac);
    snprintf(m.hostname, sizeof m.hostname, "%s", c->hostname);
    if (c->state == INKWELL_DHCP_REQUESTING) {
        m.broadcast = 1;
        memcpy(m.requested, c->offered, 4);
        memcpy(m.server, c->server, 4);
    } else if (c->state == INKWELL_DHCP_RENEWING || c->state == INKWELL_DHCP_REBINDING) {
        memcpy(m.client, c->offered, 4); /* it has the address: it says so, and asks no other */
    } else {
        m.broadcast = 1;
    }
    uint8_t bytes[INKWELL_DHCP_MESSAGE_MIN];
    const int len = inkwell_dhcp_encode(&m, bytes, sizeof bytes);
    if (len > 0) {
        (void)c->ops.send(c->ctx, bytes, (size_t)len);
    }
}

static void select_again(struct inkwell_dhcp_client *c) {
    c->state = INKWELL_DHCP_SELECTING;
    c->xid = new_xid();
    c->wait_ms = c->config.retry_ms;
    c->tries = 0U;
    c->started = inkwell_time_monotonic_ms();
    memset(c->offered, 0, sizeof c->offered);
    memset(c->server, 0, sizeof c->server);
    send_message(c, INKWELL_DHCP_DISCOVER);
    arm(c, next_wait(c));
}

/* Runs a callback; nonzero when it stopped the client, which must then not be touched. */
static int lost(struct inkwell_dhcp_client *c) {
    if (c->ops.lost == NULL) {
        return 0;
    }
    int stopped = 0;
    c->stopped = &stopped;
    c->ops.lost(c->ctx);
    if (stopped) {
        return 1;
    }
    c->stopped = NULL;
    return 0;
}

/* Milliseconds since the lease was given, and the three marks along it. */
static uint64_t held_ms(const struct inkwell_dhcp_client *c) {
    return inkwell_time_monotonic_ms() - c->bound_at;
}

/* Renewing or rebinding: another try, and the wait - never past the next mark. */
static void retry_lease(struct inkwell_dhcp_client *c) {
    const uint64_t held = held_ms(c);
    const uint64_t end = (uint64_t)c->lease.lease_s * 1000U;
    const uint64_t rebind = (uint64_t)c->rebind_s * 1000U;
    if (held >= end) {
        if (lost(c)) {
            return;
        }
        select_again(c);
        return;
    }
    if (c->state == INKWELL_DHCP_RENEWING && held >= rebind) {
        c->state = INKWELL_DHCP_REBINDING;
        c->xid = new_xid();
    }
    send_message(c, INKWELL_DHCP_REQUEST);
    const uint64_t mark = c->state == INKWELL_DHCP_RENEWING ? rebind : end;
    const uint64_t wait = next_wait(c);
    arm(c, held + wait < mark ? wait : mark - held);
}

static void on_timeout(struct inkwell_dhcp_client *c) {
    switch (c->state) {
    case INKWELL_DHCP_SELECTING:
        send_message(c, INKWELL_DHCP_DISCOVER);
        arm(c, next_wait(c));
        break;
    case INKWELL_DHCP_REQUESTING:
        if (++c->tries >= REQUEST_TRIES) {
            select_again(c);
        } else {
            send_message(c, INKWELL_DHCP_REQUEST);
            arm(c, next_wait(c));
        }
        break;
    case INKWELL_DHCP_BOUND: {
        const uint64_t held = held_ms(c);
        const uint64_t renew = (uint64_t)c->renew_s * 1000U;
        if (held < renew) {
            arm(c, renew - held); /* a long lease, waited out in pieces */
            break;
        }
        c->state = INKWELL_DHCP_RENEWING;
        c->xid = new_xid();
        c->started = inkwell_time_monotonic_ms();
        c->wait_ms = c->config.retry_ms;
        retry_lease(c);
        break;
    }
    case INKWELL_DHCP_RENEWING:
    case INKWELL_DHCP_REBINDING:
        retry_lease(c);
        break;
    case INKWELL_DHCP_STOPPED:
        break;
    }
}

static int on_timer(int fd, uint32_t events, void *userdata) {
    (void)events;
    (void)inkwell_timer_read(fd);
    on_timeout(userdata);
    return 0;
}

static void take_lease(struct inkwell_dhcp_client *c, const struct inkwell_dhcp_message *m) {
    struct inkwell_dhcp_lease *l = &c->lease;
    memset(l, 0, sizeof *l);
    memcpy(l->address, m->yours, 4);
    const int prefix = zero4(m->mask) ? 24 : inkwell_dhcp_prefix_of(m->mask);
    l->prefix = (uint8_t)(prefix < 0 ? 24 : prefix);
    memcpy(l->router, m->router, 4);
    memcpy(l->dns, m->dns, sizeof l->dns);
    l->dns_count = m->dns_count;
    memcpy(l->server, zero4(m->server) ? c->server : m->server, 4);
    /* A server that names no lease is giving one we must still renew: an hour, cautiously. */
    l->lease_s = m->lease_s != 0U ? m->lease_s : 3600U;
    if (l->lease_s == FOREVER) {
        c->renew_s = c->rebind_s = FOREVER;
    } else {
        c->renew_s = m->renew_s != 0U && m->renew_s < l->lease_s ? m->renew_s : l->lease_s / 2U;
        c->rebind_s = m->rebind_s != 0U && m->rebind_s < l->lease_s && m->rebind_s > c->renew_s
                          ? m->rebind_s
                          : (uint32_t)((uint64_t)l->lease_s * 7U / 8U);
    }
    memcpy(c->offered, l->address, 4);
    memcpy(c->server, l->server, 4);
}

int inkwell_dhcp_client_receive(struct inkwell_dhcp_client *c, const uint8_t *bytes, size_t len) {
    if (c == NULL || bytes == NULL) {
        return -EINVAL;
    }
    struct inkwell_dhcp_message m;
    const int decoded = inkwell_dhcp_decode(bytes, len, &m);
    if (decoded != 0) {
        return decoded;
    }
    if (c->loop == NULL || m.op != 2U || m.xid != c->xid ||
        memcmp(m.mac, c->config.mac, sizeof m.mac) != 0) {
        return 0; /* someone else's, or late */
    }
    const bool asking = c->state == INKWELL_DHCP_REQUESTING || c->state == INKWELL_DHCP_RENEWING ||
                        c->state == INKWELL_DHCP_REBINDING;
    if (c->state == INKWELL_DHCP_SELECTING && m.type == INKWELL_DHCP_OFFER && !zero4(m.yours)) {
        memcpy(c->offered, m.yours, 4);
        memcpy(c->server, m.server, 4);
        c->state = INKWELL_DHCP_REQUESTING;
        c->tries = 0U;
        c->wait_ms = c->config.retry_ms;
        send_message(c, INKWELL_DHCP_REQUEST);
        arm(c, next_wait(c));
    } else if (asking && m.type == INKWELL_DHCP_ACK && !zero4(m.yours)) {
        take_lease(c, &m);
        c->state = INKWELL_DHCP_BOUND;
        c->bound_at = inkwell_time_monotonic_ms();
        if (c->renew_s == FOREVER) {
            (void)inkwell_timer_disarm(c->timer);
        } else {
            arm(c, (uint64_t)c->renew_s * 1000U);
        }
        const struct inkwell_dhcp_lease lease = c->lease;
        int stopped = 0;
        c->stopped = &stopped;
        c->ops.bound(c->ctx, &lease);
        if (!stopped) {
            c->stopped = NULL;
        }
    } else if (asking && m.type == INKWELL_DHCP_NAK) {
        const bool held = c->state != INKWELL_DHCP_REQUESTING;
        if (held && lost(c)) {
            return 0;
        }
        select_again(c);
    }
    return 0;
}

int inkwell_dhcp_client_start(struct inkwell_dhcp_client *c, struct inkwell_loop *loop,
                              const struct inkwell_dhcp_config *config,
                              const struct inkwell_dhcp_ops *ops, void *ctx) {
    if (c == NULL || loop == NULL || config == NULL || ops == NULL || ops->send == NULL ||
        ops->bound == NULL || config->retry_ms == 0U || config->retry_max_ms < config->retry_ms) {
        return -EINVAL;
    }
    if (c->loop != NULL) {
        return -EBUSY;
    }
    memset(c, 0, sizeof *c);
    c->config = *config;
    snprintf(c->hostname, sizeof c->hostname, "%s",
             config->hostname != NULL ? config->hostname : "");
    c->config.hostname = c->hostname;
    c->ops = *ops;
    c->ctx = ctx;
    c->timer = inkwell_timer_open();
    if (c->timer < 0) {
        return c->timer;
    }
    const int added = inkwell_loop_add_fd(loop, c->timer, INKWELL_LOOP_IN, on_timer, c);
    if (added < 0) {
        inkwell_timer_close(c->timer);
        c->timer = -1;
        return added;
    }
    c->loop = loop;
    select_again(c);
    return 0;
}

void inkwell_dhcp_client_stop(struct inkwell_dhcp_client *c) {
    if (c == NULL || c->loop == NULL) {
        return;
    }
    if (c->stopped != NULL) {
        *c->stopped = 1;
        c->stopped = NULL;
    }
    (void)inkwell_loop_remove_fd(c->loop, c->timer);
    inkwell_timer_close(c->timer);
    c->timer = -1;
    c->loop = NULL;
    c->state = INKWELL_DHCP_STOPPED;
}
