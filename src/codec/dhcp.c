#include "inkwell/codec/dhcp.h"

#include <errno.h>
#include <string.h>

#define HEADER_LEN 236U /* op .. file, before the magic cookie */
#define COOKIE_LEN 4U

static const uint8_t k_cookie[COOKIE_LEN] = {99, 130, 83, 99};

enum {
    OPT_PAD = 0,
    OPT_MASK = 1,
    OPT_ROUTER = 3,
    OPT_DNS = 6,
    OPT_HOSTNAME = 12,
    OPT_REQUESTED = 50,
    OPT_LEASE = 51,
    OPT_TYPE = 53,
    OPT_SERVER = 54,
    OPT_PARAMS = 55,
    OPT_MAX_SIZE = 57,
    OPT_RENEW = 58,
    OPT_REBIND = 59,
    OPT_CLIENT_ID = 61,
    OPT_END = 255,
};

static int zero4(const uint8_t a[4]) {
    return a[0] == 0U && a[1] == 0U && a[2] == 0U && a[3] == 0U;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

struct writer {
    uint8_t *out;
    size_t size, at;
    int full;
};

static void option(struct writer *w, uint8_t code, const void *value, size_t len) {
    if (w->full || len > 255U || w->at + 2U + len > w->size) {
        w->full = 1;
        return;
    }
    w->out[w->at++] = code;
    w->out[w->at++] = (uint8_t)len;
    memcpy(&w->out[w->at], value, len);
    w->at += len;
}

static void option32(struct writer *w, uint8_t code, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    option(w, code, b, sizeof b);
}

int inkwell_dhcp_encode(const struct inkwell_dhcp_message *m, uint8_t *out, size_t size) {
    if (m == NULL || out == NULL || m->type < INKWELL_DHCP_DISCOVER ||
        m->type > INKWELL_DHCP_INFORM) {
        return -EINVAL;
    }
    if (size < HEADER_LEN + COOKIE_LEN) {
        return -ENOSPC;
    }
    const int reply =
        m->type == INKWELL_DHCP_OFFER || m->type == INKWELL_DHCP_ACK || m->type == INKWELL_DHCP_NAK;
    memset(out, 0, HEADER_LEN);
    out[0] = reply ? 2U : 1U;
    out[1] = 1U; /* Ethernet */
    out[2] = 6U;
    put32(&out[4], m->xid);
    out[8] = (uint8_t)(m->secs >> 8);
    out[9] = (uint8_t)m->secs;
    out[10] = m->broadcast ? 0x80U : 0U;
    memcpy(&out[12], m->client, 4);
    memcpy(&out[16], m->yours, 4);
    memcpy(&out[28], m->mac, 6);
    memcpy(&out[HEADER_LEN], k_cookie, COOKIE_LEN);

    struct writer w = {out, size, HEADER_LEN + COOKIE_LEN, 0};
    const uint8_t type = (uint8_t)m->type;
    option(&w, OPT_TYPE, &type, 1);
    if (!reply) {
        uint8_t id[7] = {1};
        memcpy(&id[1], m->mac, 6);
        option(&w, OPT_CLIENT_ID, id, sizeof id);
        const uint8_t max[2] = {(uint8_t)(INKWELL_DHCP_MESSAGE_MAX >> 8),
                                (uint8_t)INKWELL_DHCP_MESSAGE_MAX};
        option(&w, OPT_MAX_SIZE, max, sizeof max);
    }
    if (!zero4(m->requested)) {
        option(&w, OPT_REQUESTED, m->requested, 4);
    }
    if (!zero4(m->server)) {
        option(&w, OPT_SERVER, m->server, 4);
    }
    if (m->hostname[0] != '\0') {
        option(&w, OPT_HOSTNAME, m->hostname, strnlen(m->hostname, sizeof m->hostname));
    }
    if (!reply) {
        static const uint8_t params[] = {OPT_MASK,  OPT_ROUTER, OPT_DNS,
                                         OPT_LEASE, OPT_RENEW,  OPT_REBIND};
        option(&w, OPT_PARAMS, params, sizeof params);
    } else {
        if (!zero4(m->mask)) {
            option(&w, OPT_MASK, m->mask, 4);
        }
        if (!zero4(m->router)) {
            option(&w, OPT_ROUTER, m->router, 4);
        }
        if (m->dns_count > 0U) {
            const size_t n =
                m->dns_count > INKWELL_DHCP_DNS_MAX ? INKWELL_DHCP_DNS_MAX : m->dns_count;
            option(&w, OPT_DNS, m->dns, n * 4U);
        }
        if (m->lease_s != 0U) {
            option32(&w, OPT_LEASE, m->lease_s);
        }
        if (m->renew_s != 0U) {
            option32(&w, OPT_RENEW, m->renew_s);
        }
        if (m->rebind_s != 0U) {
            option32(&w, OPT_REBIND, m->rebind_s);
        }
    }
    if (w.full || w.at + 1U > size) {
        return -ENOSPC;
    }
    out[w.at++] = OPT_END;
    /* Pad to the BOOTP minimum: some servers drop a shorter request. */
    while (w.at < 300U && w.at < size) {
        out[w.at++] = OPT_PAD;
    }
    return (int)w.at;
}

int inkwell_dhcp_decode(const uint8_t *b, size_t len, struct inkwell_dhcp_message *out) {
    if (b == NULL || out == NULL || len < HEADER_LEN + COOKIE_LEN || b[1] != 1U || b[2] != 6U ||
        memcmp(&b[HEADER_LEN], k_cookie, COOKIE_LEN) != 0) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);
    out->op = b[0];
    out->xid = get32(&b[4]);
    out->secs = (uint16_t)((b[8] << 8) | b[9]);
    out->broadcast = (b[10] & 0x80U) != 0U;
    memcpy(out->client, &b[12], 4);
    memcpy(out->yours, &b[16], 4);
    memcpy(out->mac, &b[28], 6);
    size_t at = HEADER_LEN + COOKIE_LEN;
    int ended = 0;
    while (at < len && !ended) {
        const uint8_t code = b[at++];
        if (code == OPT_PAD) {
            continue;
        }
        if (code == OPT_END) {
            ended = 1;
            continue;
        }
        if (at >= len || at + 1U + b[at] > len) {
            return -EINVAL;
        }
        const size_t n = b[at++];
        const uint8_t *v = &b[at];
        at += n;
        switch (code) {
        case OPT_TYPE:
            if (n == 1U) {
                out->type = (enum inkwell_dhcp_type)v[0];
            }
            break;
        case OPT_MASK:
            if (n == 4U) {
                memcpy(out->mask, v, 4);
            }
            break;
        case OPT_ROUTER:
            if (n >= 4U) {
                memcpy(out->router, v, 4);
            }
            break;
        case OPT_DNS:
            for (size_t i = 0U; i + 4U <= n && out->dns_count < INKWELL_DHCP_DNS_MAX; i += 4U) {
                memcpy(out->dns[out->dns_count++], &v[i], 4);
            }
            break;
        case OPT_REQUESTED:
            if (n == 4U) {
                memcpy(out->requested, v, 4);
            }
            break;
        case OPT_SERVER:
            if (n == 4U) {
                memcpy(out->server, v, 4);
            }
            break;
        case OPT_LEASE:
            if (n == 4U) {
                out->lease_s = get32(v);
            }
            break;
        case OPT_RENEW:
            if (n == 4U) {
                out->renew_s = get32(v);
            }
            break;
        case OPT_REBIND:
            if (n == 4U) {
                out->rebind_s = get32(v);
            }
            break;
        case OPT_HOSTNAME: {
            const size_t keep = n < sizeof out->hostname - 1U ? n : sizeof out->hostname - 1U;
            memcpy(out->hostname, v, keep);
            out->hostname[keep] = '\0';
            break;
        }
        default:
            break;
        }
    }
    if (out->type < INKWELL_DHCP_DISCOVER || out->type > INKWELL_DHCP_INFORM) {
        return -EINVAL;
    }
    return 0;
}

int inkwell_dhcp_prefix_of(const uint8_t mask[4]) {
    const uint32_t m = get32(mask);
    int prefix = 0;
    while (prefix < 32 && (m & (0x80000000U >> prefix)) != 0U) {
        ++prefix;
    }
    /* Every bit after the ones must be zero. */
    const uint32_t ones = prefix == 0 ? 0U : 0xFFFFFFFFU << (32 - prefix);
    return m == ones ? prefix : -EINVAL;
}
