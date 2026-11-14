/*
 * DHCPv4 messages: a client's DISCOVER and REQUEST as bytes a server reads, a server's OFFER and
 * ACK read back, and the shapes that are not messages refused.
 */
#include "framework/inkwell_test.h"

#include "inkwell/codec/dhcp.h"

#include <errno.h>
#include <string.h>

static const uint8_t k_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

INKWELL_TEST_CASE(dhcp_encodes_a_discover_a_server_reads, unit) {
    struct inkwell_dhcp_message m;
    memset(&m, 0, sizeof m);
    m.type = INKWELL_DHCP_DISCOVER;
    m.xid = 0xA1B2C3D4U;
    m.broadcast = 1;
    memcpy(m.mac, k_mac, 6);
    strcpy(m.hostname, "phone");
    uint8_t bytes[INKWELL_DHCP_MESSAGE_MIN];
    const int len = inkwell_dhcp_encode(&m, bytes, sizeof bytes);
    INKWELL_TEST_FAIL_IF(len < 300, "padded to BOOTP's minimum");
    INKWELL_TEST_FAIL_IF(bytes[0] != 1U || bytes[1] != 1U || bytes[2] != 6U,
                         "a request, over Ethernet, six-byte addresses");
    INKWELL_TEST_FAIL_IF(bytes[4] != 0xA1U || bytes[7] != 0xD4U, "the transaction, big-endian");
    INKWELL_TEST_FAIL_IF(bytes[10] != 0x80U, "the broadcast flag");
    INKWELL_TEST_FAIL_IF(memcmp(&bytes[28], k_mac, 6) != 0, "chaddr");
    INKWELL_TEST_FAIL_IF(bytes[236] != 99U || bytes[239] != 99U, "the magic cookie");
    INKWELL_TEST_FAIL_IF(bytes[240] != 53U || bytes[241] != 1U || bytes[242] != 1U,
                         "the type comes first: DISCOVER");
    struct inkwell_dhcp_message back;
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, (size_t)len, &back) != 0, "it reads back");
    INKWELL_TEST_FAIL_IF(back.op != 1U || back.type != INKWELL_DHCP_DISCOVER || back.xid != m.xid ||
                             !back.broadcast || strcmp(back.hostname, "phone") != 0,
                         "as it was written");
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_encode(&m, bytes, 100U) != -ENOSPC, "a short buffer");
    m.type = (enum inkwell_dhcp_type)42;
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_encode(&m, bytes, sizeof bytes) != -EINVAL, "no such type");
    record_success(test_name);
}

INKWELL_TEST_CASE(dhcp_reads_what_a_server_offers, unit) {
    struct inkwell_dhcp_message offer;
    memset(&offer, 0, sizeof offer);
    offer.type = INKWELL_DHCP_ACK;
    offer.xid = 7U;
    memcpy(offer.mac, k_mac, 6);
    const uint8_t yours[4] = {10, 0, 2, 15}, server[4] = {10, 0, 2, 2},
                  mask[4] = {255, 255, 255, 0}, dns[4] = {10, 0, 2, 3};
    memcpy(offer.yours, yours, 4);
    memcpy(offer.server, server, 4);
    memcpy(offer.mask, mask, 4);
    memcpy(offer.router, server, 4);
    memcpy(offer.dns[0], dns, 4);
    memcpy(offer.dns[1], server, 4);
    offer.dns_count = 2U;
    offer.lease_s = 86400U;
    offer.renew_s = 3600U;
    uint8_t bytes[INKWELL_DHCP_MESSAGE_MIN];
    const int len = inkwell_dhcp_encode(&offer, bytes, sizeof bytes);
    INKWELL_TEST_FAIL_IF(len <= 0 || bytes[0] != 2U, "a reply");
    struct inkwell_dhcp_message m;
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, (size_t)len, &m) != 0, "decoded");
    INKWELL_TEST_FAIL_IF(m.type != INKWELL_DHCP_ACK || memcmp(m.yours, yours, 4) != 0 ||
                             memcmp(m.server, server, 4) != 0 || memcmp(m.router, server, 4) != 0,
                         "the address, the server, the router");
    INKWELL_TEST_FAIL_IF(m.dns_count != 2U || memcmp(m.dns[0], dns, 4) != 0,
                         "both name servers, in order");
    INKWELL_TEST_FAIL_IF(m.lease_s != 86400U || m.renew_s != 3600U || m.rebind_s != 0U,
                         "the times, and an absent one as 0");
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_prefix_of(m.mask) != 24, "a /24");
    record_success(test_name);
}

INKWELL_TEST_CASE(dhcp_refuses_what_is_not_a_message, unit) {
    struct inkwell_dhcp_message m;
    memset(&m, 0, sizeof m);
    m.type = INKWELL_DHCP_OFFER;
    uint8_t bytes[INKWELL_DHCP_MESSAGE_MIN];
    const int len = inkwell_dhcp_encode(&m, bytes, sizeof bytes);
    struct inkwell_dhcp_message out;
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, 200U, &out) != -EINVAL, "too short");
    bytes[237] = 0U;
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, (size_t)len, &out) != -EINVAL, "no cookie");
    bytes[237] = 130U;
    bytes[243] = 6U;   /* an option after the type... */
    bytes[244] = 200U; /* ...that says it runs past the end */
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, 246U, &out) != -EINVAL, "an option overruns");
    bytes[240] = 255U; /* END before any type */
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_decode(bytes, (size_t)len, &out) != -EINVAL, "no type");
    const uint8_t holey[4] = {255, 0, 255, 0}, all[4] = {255, 255, 255, 255}, none[4] = {0};
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_prefix_of(holey) != -EINVAL, "a mask with a hole");
    INKWELL_TEST_FAIL_IF(inkwell_dhcp_prefix_of(all) != 32 || inkwell_dhcp_prefix_of(none) != 0,
                         "the two ends");
    record_success(test_name);
}
