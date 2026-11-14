#pragma once

/*
 * The system's network interfaces, as the kernel keeps them: the links (an Ethernet port, a
 * wireless one, loopback), the addresses on each and the default route - read, changed and
 * watched over Linux's rtnetlink socket. It is what `ip link`, `ip addr` and `ip route` do, as
 * calls rather than programs.
 *
 * A change is a request to the kernel that it answers at once, so the calls that change something
 * wait for that answer (a second at most) rather than taking a callback; nothing here waits on the
 * network itself. What does wait - a cable plugged in, an address given - is heard through the
 * monitor, which says only *that* something changed: read the state again, as uevent.h's
 * monitor says to look at /sys. A burst of changes is one wakeup, and changes lost to a full
 * buffer are reported the same way, so nothing is missed by acting on what the state is now.
 *
 * IPv4 only, for now: what a DHCPv4 lease (net/dhcp.h) applies.
 *
 * Every change needs CAP_NET_ADMIN, and acts in the network namespace the process is in. Off
 * Linux every call refuses with -ENOSYS.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

#define INKWELL_NETIF_NAME_MAX 16U /* IFNAMSIZ */

struct inkwell_netif_link {
    int index;
    char name[INKWELL_NETIF_NAME_MAX];
    bool up;      /* administratively: IFF_UP */
    bool carrier; /* something is on the other end: IFF_LOWER_UP */
    bool loopback;
    uint8_t mac[6]; /* zeros for a link without one */
    uint32_t mtu;
};

struct inkwell_netif_address {
    int index;
    uint8_t address[4];
    uint8_t prefix;
};

/* Up to `max` links into `out`, in the kernel's order. Returns how many there are in all - more
   than `max` when some did not fit - or a negative errno. */
int inkwell_netif_links(struct inkwell_netif_link *out, size_t max);

/* The link called `name`. 0, -ENODEV for none, or a negative errno. */
int inkwell_netif_link_named(const char *name, struct inkwell_netif_link *out);

/* Brings link `index` up or down. 0 or a negative errno. */
int inkwell_netif_set_up(int index, bool up);

/* The IPv4 addresses on link `index` (0: on every link), as inkwell_netif_links() counts. */
int inkwell_netif_addresses(int index, struct inkwell_netif_address *out, size_t max);

/* `address`/`prefix` on link `index`, beside any already there. 0 or a negative errno; an address
   it already has is 0. */
int inkwell_netif_address_add(int index, const uint8_t address[4], uint8_t prefix);

/* Every IPv4 address off link `index`. 0 or a negative errno. */
int inkwell_netif_address_flush(int index);

/* The default route: through `gateway` on link `index`, replacing any other. 0 or a negative
   errno. */
int inkwell_netif_route_default(int index, const uint8_t gateway[4]);

/* What changed. Lost changes are all of them. */
enum {
    INKWELL_NETIF_LINKS = 1,
    INKWELL_NETIF_ADDRESSES = 2,
    INKWELL_NETIF_ROUTES = 4,
};

typedef void (*inkwell_netif_callback)(unsigned changed, void *userdata);

struct inkwell_netif_monitor {
    struct inkwell_loop *loop; /* NULL while closed */
    int fd;
    inkwell_netif_callback callback;
    void *userdata;
    int *closed; /* as uevent.h's: set by a close from inside the callback. Private. */
};

/* Watches links, IPv4 addresses and routes on `loop`. 0, or a negative errno with the monitor
   closed. A zeroed monitor is a closed one. */
int inkwell_netif_monitor_open(struct inkwell_netif_monitor *monitor, struct inkwell_loop *loop,
                               inkwell_netif_callback callback, void *userdata);

/* Safe on a closed monitor, and from inside its own callback. */
void inkwell_netif_monitor_close(struct inkwell_netif_monitor *monitor);

/*
 * A socket for a DHCP client on link `name` (net/dhcp.h): UDP, non-blocking, on the client port
 * (68), bound to that link alone, allowed to broadcast though the link has no address yet. Send
 * with inkwell_netif_dhcp_send(); read it when the loop says it is readable. The descriptor, or a
 * negative errno.
 */
int inkwell_netif_dhcp_socket(const char *name);

/* `len` bytes broadcast to the DHCP servers (255.255.255.255:67) on that socket. 0 or a negative
   errno. Fits net/dhcp.h's `send` once a context carries the descriptor. */
int inkwell_netif_dhcp_send(int fd, const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif
