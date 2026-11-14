#pragma once

/*
 * DHCPv4 messages (RFC 2131, options from RFC 2132): one struct both ways, so a client builds a
 * DISCOVER with the same type it reads an OFFER into - and a test plays the server with it.
 *
 * Only what a client asks for and acts on is carried: the address offered, the server that
 * offered it, the subnet mask, a router, up to three name servers, and the three times (lease,
 * renew, rebind). Everything else in a reply is skipped, and a client never needs it. An address
 * of 0.0.0.0 is "absent" throughout: none of these is ever meaningfully all zeros.
 *
 * Plain bytes: no socket and no clock, so it builds and is tested on every system. The client
 * that sends and times these is net/dhcp.h.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Option 53. */
enum inkwell_dhcp_type {
    INKWELL_DHCP_DISCOVER = 1,
    INKWELL_DHCP_OFFER = 2,
    INKWELL_DHCP_REQUEST = 3,
    INKWELL_DHCP_DECLINE = 4,
    INKWELL_DHCP_ACK = 5,
    INKWELL_DHCP_NAK = 6,
    INKWELL_DHCP_RELEASE = 7,
    INKWELL_DHCP_INFORM = 8,
};

#define INKWELL_DHCP_DNS_MAX 3U
#define INKWELL_DHCP_HOSTNAME_MAX 64U
/* What a client must be able to receive (RFC 2131 section 2): 576 bytes of IP, less 28 of IP
   and UDP headers. Replies may be longer; a buffer this big takes every one a client asks for. */
#define INKWELL_DHCP_MESSAGE_MIN 548U
#define INKWELL_DHCP_MESSAGE_MAX 1500U

/* Ports, for whoever owns the socket. */
#define INKWELL_DHCP_SERVER_PORT 67U
#define INKWELL_DHCP_CLIENT_PORT 68U

struct inkwell_dhcp_message {
    uint8_t op; /* 1: client to server (BOOTREQUEST), 2: the reply. encode() picks it by type. */
    enum inkwell_dhcp_type type;
    uint32_t xid; /* the transaction a reply answers */
    uint16_t secs;
    int broadcast; /* nonzero: the client asks for its reply broadcast (it has no address yet) */
    uint8_t mac[6];
    uint8_t client[4];                    /* ciaddr: the client's address while it renews */
    uint8_t yours[4];                     /* yiaddr: the address offered or confirmed */
    uint8_t requested[4];                 /* option 50 */
    uint8_t server[4];                    /* option 54: who offered, and who a REQUEST is for */
    uint8_t mask[4];                      /* option 1 */
    uint8_t router[4];                    /* option 3, the first one */
    uint8_t dns[INKWELL_DHCP_DNS_MAX][4]; /* option 6 */
    uint8_t dns_count;
    uint32_t lease_s;                         /* option 51; 0 absent, 0xFFFFFFFF for ever */
    uint32_t renew_s;                         /* option 58 (T1); 0 absent */
    uint32_t rebind_s;                        /* option 59 (T2); 0 absent */
    char hostname[INKWELL_DHCP_HOSTNAME_MAX]; /* option 12; "" for none */
};

/*
 * `message` as bytes into `out`: the BOOTP header, the magic cookie and the options it carries -
 * and, from a client, a parameter request list for the options above. Returns the length, or
 * -EINVAL for a type it does not know, -ENOSPC when `size` is short (INKWELL_DHCP_MESSAGE_MIN is
 * always enough).
 */
int inkwell_dhcp_encode(const struct inkwell_dhcp_message *message, uint8_t *out, size_t size);

/* `len` bytes into `out`. 0, or -EINVAL for anything that is not a DHCP message: short, no magic
   cookie, a hardware type other than Ethernet, no message type, or an option running off the
   end. */
int inkwell_dhcp_decode(const uint8_t *bytes, size_t len, struct inkwell_dhcp_message *out);

/* The prefix length a subnet mask says (255.255.255.0 is 24), or -EINVAL for a mask whose ones
   are not contiguous. */
int inkwell_dhcp_prefix_of(const uint8_t mask[4]);

#ifdef __cplusplus
}
#endif
