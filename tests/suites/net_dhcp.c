/*
 * A DHCP client against a server played in-process: retries until answered, takes an offer,
 * renews before its lease runs out, starts again when a server says no, and gives the lease up
 * when nobody answers. The client owns no socket (net/dhcp.h), so this runs on every system.
 */
#include "framework/inkwell_test.h"

#include "inkwell/net/dhcp.h"
#include "inkwell/runtime/loop.h"

#include <string.h>

struct server {
    struct inkwell_dhcp_message last; /* the client's last message */
    int sent;
    int bound;
    int lost;
    struct inkwell_dhcp_lease lease;
};

static struct server g_server;
static struct inkwell_dhcp_client g_client;

static int on_send(void *ctx, const uint8_t *bytes, size_t len) {
    struct server *s = ctx;
    if (inkwell_dhcp_decode(bytes, len, &s->last) == 0) {
        s->sent++;
    }
    return 0;
}

static void on_bound(void *ctx, const struct inkwell_dhcp_lease *lease) {
    struct server *s = ctx;
    s->bound++;
    s->lease = *lease;
}

static void on_lost(void *ctx) {
    ((struct server *)ctx)->lost++;
}

static const struct inkwell_dhcp_ops k_ops = {on_send, on_bound, on_lost};
static const uint8_t k_mac[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t k_yours[4] = {10, 0, 2, 15};
static const uint8_t k_server[4] = {10, 0, 2, 2};

/* The server's answer to the client's last message. */
static void answer(enum inkwell_dhcp_type type, uint32_t lease_s) {
    struct inkwell_dhcp_message m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.xid = g_server.last.xid;
    memcpy(m.mac, k_mac, 6);
    if (type != INKWELL_DHCP_NAK) {
        memcpy(m.yours, k_yours, 4);
        const uint8_t mask[4] = {255, 255, 255, 0};
        memcpy(m.mask, mask, 4);
        memcpy(m.router, k_server, 4);
        memcpy(m.dns[0], k_server, 4);
        m.dns_count = 1U;
        m.lease_s = lease_s;
    }
    memcpy(m.server, k_server, 4);
    uint8_t bytes[INKWELL_DHCP_MESSAGE_MIN];
    const int len = inkwell_dhcp_encode(&m, bytes, sizeof bytes);
    (void)inkwell_dhcp_client_receive(&g_client, bytes, (size_t)len);
}

static void run_until(struct inkwell_loop *loop, const int *counter, int want, int max_ms) {
    for (int i = 0; i < max_ms / 5 && *counter < want; ++i) {
        (void)inkwell_loop_run(loop, 5);
    }
}

static void start(struct inkwell_loop *loop) {
    memset(&g_server, 0, sizeof g_server);
    memset(&g_client, 0, sizeof g_client);
    struct inkwell_dhcp_config config;
    memset(&config, 0, sizeof config);
    memcpy(config.mac, k_mac, 6);
    config.hostname = "phone";
    config.retry_ms = 20U;
    config.retry_max_ms = 80U;
    (void)inkwell_loop_init(loop);
    (void)inkwell_dhcp_client_start(&g_client, loop, &config, &k_ops, &g_server);
}

INKWELL_TEST_CASE(dhcp_client_asks_until_answered_and_takes_the_offer, unit) {
    struct inkwell_loop loop;
    start(&loop);
    INKWELL_TEST_FAIL_IF(g_server.sent != 1 || g_server.last.type != INKWELL_DHCP_DISCOVER,
                         "a DISCOVER at once");
    run_until(&loop, &g_server.sent, 3, 1000);
    INKWELL_TEST_FAIL_IF(g_server.sent < 3 || g_server.last.type != INKWELL_DHCP_DISCOVER,
                         "and again, unanswered");
    const uint32_t xid = g_server.last.xid;

    /* Someone else's offer is not this client's. */
    g_server.last.xid = xid + 1U;
    const int before = g_server.sent;
    answer(INKWELL_DHCP_OFFER, 3600U);
    INKWELL_TEST_FAIL_IF(g_server.sent != before, "another transaction's offer is ignored");
    g_server.last.xid = xid;

    answer(INKWELL_DHCP_OFFER, 3600U);
    INKWELL_TEST_FAIL_IF(g_server.last.type != INKWELL_DHCP_REQUEST ||
                             memcmp(g_server.last.requested, k_yours, 4) != 0 ||
                             memcmp(g_server.last.server, k_server, 4) != 0 ||
                             g_server.last.xid != xid,
                         "a REQUEST for what was offered, to whoever offered it");
    answer(INKWELL_DHCP_ACK, 3600U);
    INKWELL_TEST_FAIL_IF(g_server.bound != 1 || g_client.state != INKWELL_DHCP_BOUND,
                         "bound, and said so");
    INKWELL_TEST_FAIL_IF(memcmp(g_server.lease.address, k_yours, 4) != 0 ||
                             g_server.lease.prefix != 24U ||
                             memcmp(g_server.lease.router, k_server, 4) != 0 ||
                             g_server.lease.dns_count != 1U || g_server.lease.lease_s != 3600U,
                         "the lease as the server gave it");
    const int quiet = g_server.sent;
    for (int i = 0; i < 20; ++i) {
        (void)inkwell_loop_run(&loop, 5);
    }
    INKWELL_TEST_FAIL_IF(g_server.sent != quiet, "bound for an hour: nothing more is sent");
    inkwell_dhcp_client_stop(&g_client);
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

INKWELL_TEST_CASE(dhcp_client_renews_and_starts_again_on_a_no, unit) {
    struct inkwell_loop loop;
    start(&loop);
    answer(INKWELL_DHCP_OFFER, 2U);
    answer(INKWELL_DHCP_ACK, 2U);
    INKWELL_TEST_FAIL_IF(g_server.bound != 1, "a two-second lease");
    const int sent = g_server.sent;
    run_until(&loop, &g_server.sent, sent + 1, 1500);
    INKWELL_TEST_FAIL_IF(g_server.last.type != INKWELL_DHCP_REQUEST ||
                             memcmp(g_server.last.client, k_yours, 4) != 0 ||
                             g_server.last.requested[0] != 0U,
                         "half way through, a renewal: the address it holds, nothing asked");
    answer(INKWELL_DHCP_ACK, 2U);
    INKWELL_TEST_FAIL_IF(g_server.bound != 2 || g_client.state != INKWELL_DHCP_BOUND, "renewed");
    run_until(&loop, &g_server.sent, g_server.sent + 1, 1500);
    answer(INKWELL_DHCP_NAK, 0U);
    INKWELL_TEST_FAIL_IF(g_server.lost != 1 || g_server.last.type != INKWELL_DHCP_DISCOVER,
                         "a server's no: lost, and a DISCOVER");
    inkwell_dhcp_client_stop(&g_client);
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

INKWELL_TEST_CASE(dhcp_client_gives_the_lease_up_when_nobody_answers, unit) {
    struct inkwell_loop loop;
    start(&loop);
    answer(INKWELL_DHCP_OFFER, 2U);
    answer(INKWELL_DHCP_ACK, 2U);
    run_until(&loop, &g_server.lost, 1, 3000);
    INKWELL_TEST_FAIL_IF(g_server.lost != 1, "the lease ran out unanswered");
    INKWELL_TEST_FAIL_IF(g_client.state != INKWELL_DHCP_SELECTING ||
                             g_server.last.type != INKWELL_DHCP_DISCOVER,
                         "and it asks again from the start");
    inkwell_dhcp_client_stop(&g_client);
    INKWELL_TEST_FAIL_IF(g_client.state != INKWELL_DHCP_STOPPED, "stopped");
    inkwell_dhcp_client_stop(&g_client); /* twice is fine */
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}
