#include "inkwell/io/netif.h"

#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REPLY_WAIT_MS 1000
#define BUFFER 16384
#define REQUEST_MAX 256
#define READ_BUDGET 16

/* One request, built in place: the header, the family's message, then attributes. */
struct request {
    union {
        struct nlmsghdr header;
        uint8_t bytes[REQUEST_MAX];
    } u;
};

static void *request_start(struct request *r, uint16_t type, uint16_t flags, size_t body) {
    memset(r, 0, sizeof *r);
    r->u.header.nlmsg_len = (uint32_t)NLMSG_LENGTH(body);
    r->u.header.nlmsg_type = type;
    r->u.header.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags);
    return NLMSG_DATA(&r->u.header);
}

static void attribute(struct request *r, uint16_t type, const void *value, size_t len) {
    const size_t at = NLMSG_ALIGN(r->u.header.nlmsg_len);
    struct rtattr *a = (struct rtattr *)(void *)&r->u.bytes[at];
    a->rta_type = type;
    a->rta_len = (uint16_t)RTA_LENGTH(len);
    memcpy(RTA_DATA(a), value, len);
    r->u.header.nlmsg_len = (uint32_t)(at + RTA_ALIGN(a->rta_len));
}

/* The attributes after a message's fixed part: the kernel's RTA_OK/RTA_NEXT, without their signed
   length arithmetic. */
static const struct rtattr *attr_first(const struct nlmsghdr *m, size_t fixed, size_t *left) {
    const size_t start = NLMSG_LENGTH(fixed);
    const size_t at = NLMSG_ALIGN(start);
    *left = m->nlmsg_len > at ? m->nlmsg_len - at : 0U;
    return (const struct rtattr *)(const void *)((const uint8_t *)m + at);
}

static const struct rtattr *attr_next(const struct rtattr *a, size_t *left) {
    if (*left < sizeof *a || a->rta_len < sizeof *a || a->rta_len > *left) {
        *left = 0U;
        return NULL;
    }
    const size_t step = RTA_ALIGN((size_t)a->rta_len);
    *left = step < *left ? *left - step : 0U;
    return (const struct rtattr *)(const void *)((const uint8_t *)a + step);
}

static bool attr_ok(const struct rtattr *a, size_t left) {
    return a != NULL && left >= sizeof *a && a->rta_len >= sizeof *a && a->rta_len <= left;
}

typedef void (*each_fn)(const struct nlmsghdr *message, void *ctx);

/*
 * Sends `r` and reads the kernel's answer: each message of a dump to `each`, until DONE; or the
 * acknowledgement of a change, whose error is the result. 0 or a negative errno.
 */
static int talk(struct request *r, each_fn each, void *ctx) {
    const int fd = inkwell_fd_socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        return fd;
    }
    static uint32_t seq;
    r->u.header.nlmsg_seq = ++seq;
    struct sockaddr_nl kernel;
    memset(&kernel, 0, sizeof kernel);
    kernel.nl_family = AF_NETLINK;
    int result = 0;
    if (sendto(fd, r->u.bytes, r->u.header.nlmsg_len, 0, (const struct sockaddr *)&kernel,
               sizeof kernel) < 0) {
        result = -errno;
        (void)close(fd);
        return result;
    }
    static uint8_t buf[BUFFER];
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        const int ready = poll(&p, 1, REPLY_WAIT_MS);
        if (ready <= 0) {
            result = ready == 0 ? -ETIMEDOUT : -errno;
            if (result == -EINTR) {
                continue;
            }
            break;
        }
        const ssize_t got = recv(fd, buf, sizeof buf, 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            result = -errno;
            break;
        }
        bool done = false;
        size_t left = (size_t)got;
        for (const struct nlmsghdr *m = (const struct nlmsghdr *)(const void *)buf;
             NLMSG_OK(m, left); m = NLMSG_NEXT(m, left)) {
            if (m->nlmsg_seq != r->u.header.nlmsg_seq) {
                continue;
            }
            if (m->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (m->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *e = NLMSG_DATA(m);
                result = e->error;
                done = true;
                break;
            }
            if (each != NULL) {
                each(m, ctx);
            }
        }
        if (done) {
            break;
        }
    }
    (void)close(fd);
    return result;
}

/* --- links ---------------------------------------------------------------------------------- */

struct link_list {
    struct inkwell_netif_link *out;
    size_t max, count;
};

static void link_of(const struct nlmsghdr *m, void *ctx) {
    struct link_list *list = ctx;
    if (m->nlmsg_type != RTM_NEWLINK) {
        return;
    }
    const struct ifinfomsg *info = NLMSG_DATA(m);
    const size_t n = list->count++;
    if (n >= list->max) {
        return;
    }
    struct inkwell_netif_link *l = &list->out[n];
    memset(l, 0, sizeof *l);
    l->index = info->ifi_index;
    l->up = (info->ifi_flags & IFF_UP) != 0U;
    l->carrier = (info->ifi_flags & IFF_LOWER_UP) != 0U;
    l->loopback = (info->ifi_flags & IFF_LOOPBACK) != 0U || info->ifi_type == ARPHRD_LOOPBACK;
    size_t len = 0U;
    for (const struct rtattr *a = attr_first(m, sizeof *info, &len); attr_ok(a, len);
         a = attr_next(a, &len)) {
        const size_t size = RTA_PAYLOAD(a);
        if (a->rta_type == IFLA_IFNAME) {
            const size_t keep = size < sizeof l->name ? size : sizeof l->name - 1U;
            memcpy(l->name, RTA_DATA(a), keep);
            l->name[sizeof l->name - 1U] = '\0';
        } else if (a->rta_type == IFLA_ADDRESS && size == sizeof l->mac) {
            memcpy(l->mac, RTA_DATA(a), sizeof l->mac);
        } else if (a->rta_type == IFLA_MTU && size == sizeof(uint32_t)) {
            memcpy(&l->mtu, RTA_DATA(a), sizeof l->mtu);
        }
    }
}

int inkwell_netif_links(struct inkwell_netif_link *out, size_t max) {
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    struct request r;
    struct ifinfomsg *info = request_start(&r, RTM_GETLINK, NLM_F_DUMP, sizeof *info);
    info->ifi_family = AF_UNSPEC;
    struct link_list list = {out, max, 0U};
    const int result = talk(&r, link_of, &list);
    return result < 0 ? result : (int)list.count;
}

int inkwell_netif_link_named(const char *name, struct inkwell_netif_link *out) {
    if (name == NULL || out == NULL) {
        return -EINVAL;
    }
    struct inkwell_netif_link links[32];
    const int n = inkwell_netif_links(links, 32U);
    if (n < 0) {
        return n;
    }
    for (int i = 0; i < n && i < 32; ++i) {
        if (strcmp(links[i].name, name) == 0) {
            *out = links[i];
            return 0;
        }
    }
    return -ENODEV;
}

int inkwell_netif_set_up(int index, bool up) {
    if (index <= 0) {
        return -EINVAL;
    }
    struct request r;
    struct ifinfomsg *info = request_start(&r, RTM_NEWLINK, NLM_F_ACK, sizeof *info);
    info->ifi_family = AF_UNSPEC;
    info->ifi_index = index;
    info->ifi_flags = up ? IFF_UP : 0U;
    info->ifi_change = IFF_UP;
    return talk(&r, NULL, NULL);
}

/* --- addresses ------------------------------------------------------------------------------ */

struct address_list {
    int index;
    struct inkwell_netif_address *out;
    size_t max, count;
};

static void address_of(const struct nlmsghdr *m, void *ctx) {
    struct address_list *list = ctx;
    if (m->nlmsg_type != RTM_NEWADDR) {
        return;
    }
    const struct ifaddrmsg *info = NLMSG_DATA(m);
    if (info->ifa_family != AF_INET || (list->index != 0 && (int)info->ifa_index != list->index)) {
        return;
    }
    struct inkwell_netif_address found;
    memset(&found, 0, sizeof found);
    found.index = (int)info->ifa_index;
    found.prefix = info->ifa_prefixlen;
    size_t len = 0U;
    bool have = false;
    for (const struct rtattr *a = attr_first(m, sizeof *info, &len); attr_ok(a, len);
         a = attr_next(a, &len)) {
        /* LOCAL is the interface's own address; ADDRESS is the peer's on a point-to-point
           link, and the same as LOCAL elsewhere. */
        if ((a->rta_type == IFA_LOCAL || (a->rta_type == IFA_ADDRESS && !have)) &&
            RTA_PAYLOAD(a) == 4U) {
            memcpy(found.address, RTA_DATA(a), 4);
            have = true;
        }
    }
    const size_t n = list->count++;
    if (have && n < list->max) {
        list->out[n] = found;
    }
}

int inkwell_netif_addresses(int index, struct inkwell_netif_address *out, size_t max) {
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    struct request r;
    struct ifaddrmsg *info = request_start(&r, RTM_GETADDR, NLM_F_DUMP, sizeof *info);
    info->ifa_family = AF_INET;
    struct address_list list = {index, out, max, 0U};
    const int result = talk(&r, address_of, &list);
    return result < 0 ? result : (int)list.count;
}

static int address_change(uint16_t type, uint16_t flags, int index, const uint8_t address[4],
                          uint8_t prefix) {
    if (index <= 0 || address == NULL || prefix > 32U) {
        return -EINVAL;
    }
    struct request r;
    struct ifaddrmsg *info = request_start(&r, type, (uint16_t)(NLM_F_ACK | flags), sizeof *info);
    info->ifa_family = AF_INET;
    info->ifa_prefixlen = prefix;
    info->ifa_scope = RT_SCOPE_UNIVERSE;
    info->ifa_index = (uint32_t)index;
    attribute(&r, IFA_LOCAL, address, 4);
    attribute(&r, IFA_ADDRESS, address, 4);
    if (type == RTM_NEWADDR && prefix < 31U) {
        uint8_t broadcast[4];
        const uint32_t host = prefix == 0U ? 0xFFFFFFFFU : 0xFFFFFFFFU >> prefix;
        for (int i = 0; i < 4; ++i) {
            broadcast[i] = (uint8_t)(address[i] | (uint8_t)(host >> (24 - 8 * i)));
        }
        attribute(&r, IFA_BROADCAST, broadcast, 4);
    }
    return talk(&r, NULL, NULL);
}

int inkwell_netif_address_add(int index, const uint8_t address[4], uint8_t prefix) {
    const int r = address_change(RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE, index, address, prefix);
    return r == -EEXIST ? 0 : r;
}

int inkwell_netif_address_flush(int index) {
    struct inkwell_netif_address found[16];
    const int n = inkwell_netif_addresses(index, found, 16U);
    if (n < 0) {
        return n;
    }
    for (int i = 0; i < n && i < 16; ++i) {
        const int r =
            address_change(RTM_DELADDR, 0U, found[i].index, found[i].address, found[i].prefix);
        if (r < 0 && r != -EADDRNOTAVAIL) {
            return r;
        }
    }
    return 0;
}

int inkwell_netif_route_default(int index, const uint8_t gateway[4]) {
    if (index <= 0 || gateway == NULL) {
        return -EINVAL;
    }
    struct request r;
    struct rtmsg *info =
        request_start(&r, RTM_NEWROUTE, NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE, sizeof *info);
    info->rtm_family = AF_INET;
    info->rtm_dst_len = 0;
    info->rtm_table = RT_TABLE_MAIN;
    info->rtm_protocol = RTPROT_BOOT;
    info->rtm_scope = RT_SCOPE_UNIVERSE;
    info->rtm_type = RTN_UNICAST;
    const uint32_t oif = (uint32_t)index;
    attribute(&r, RTA_GATEWAY, gateway, 4);
    attribute(&r, RTA_OIF, &oif, sizeof oif);
    return talk(&r, NULL, NULL);
}

/* --- the monitor ---------------------------------------------------------------------------- */

static unsigned kind_of(uint16_t type) {
    switch (type) {
    case RTM_NEWLINK:
    case RTM_DELLINK:
        return INKWELL_NETIF_LINKS;
    case RTM_NEWADDR:
    case RTM_DELADDR:
        return INKWELL_NETIF_ADDRESSES;
    case RTM_NEWROUTE:
    case RTM_DELROUTE:
        return INKWELL_NETIF_ROUTES;
    default:
        return 0U;
    }
}

static int on_readable(int fd, uint32_t events, void *userdata) {
    (void)events;
    struct inkwell_netif_monitor *monitor = userdata;
    static uint8_t buf[BUFFER];
    unsigned changed = 0U;
    for (int taken = 0; taken < READ_BUDGET; ++taken) {
        const ssize_t got = recv(fd, buf, sizeof buf, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOBUFS) {
                changed |= INKWELL_NETIF_LINKS | INKWELL_NETIF_ADDRESSES | INKWELL_NETIF_ROUTES;
                continue;
            }
            break;
        }
        size_t left = (size_t)got;
        for (const struct nlmsghdr *m = (const struct nlmsghdr *)(const void *)buf;
             NLMSG_OK(m, left); m = NLMSG_NEXT(m, left)) {
            changed |= kind_of(m->nlmsg_type);
        }
    }
    if (changed != 0U) {
        int closed = 0;
        monitor->closed = &closed;
        monitor->callback(changed, monitor->userdata);
        if (!closed) {
            monitor->closed = NULL;
        }
    }
    return 0;
}

int inkwell_netif_monitor_open(struct inkwell_netif_monitor *monitor, struct inkwell_loop *loop,
                               inkwell_netif_callback callback, void *userdata) {
    if (monitor == NULL || loop == NULL || callback == NULL) {
        return -EINVAL;
    }
    if (monitor->loop != NULL) {
        return -EBUSY;
    }
    const int fd = inkwell_fd_socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        return fd;
    }
    struct sockaddr_nl address;
    memset(&address, 0, sizeof address);
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
    if (bind(fd, (const struct sockaddr *)&address, sizeof address) != 0) {
        const int error = -errno;
        (void)close(fd);
        return error;
    }
    monitor->fd = fd;
    monitor->callback = callback;
    monitor->userdata = userdata;
    monitor->closed = NULL;
    const int added = inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, on_readable, monitor);
    if (added < 0) {
        (void)close(fd);
        monitor->fd = -1;
        return added;
    }
    monitor->loop = loop;
    return 0;
}

void inkwell_netif_monitor_close(struct inkwell_netif_monitor *monitor) {
    if (monitor == NULL || monitor->loop == NULL) {
        return;
    }
    if (monitor->closed != NULL) {
        *monitor->closed = 1;
        monitor->closed = NULL;
    }
    (void)inkwell_loop_remove_fd(monitor->loop, monitor->fd);
    (void)close(monitor->fd);
    monitor->loop = NULL;
    monitor->fd = -1;
    monitor->callback = NULL;
    monitor->userdata = NULL;
}

/* --- DHCP's socket -------------------------------------------------------------------------- */

int inkwell_netif_dhcp_socket(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= INKWELL_NETIF_NAME_MAX) {
        return -EINVAL;
    }
    const int fd = inkwell_fd_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return fd;
    }
    const int on = 1;
    int result = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, (socklen_t)strlen(name) + 1U) != 0) {
        result = -errno;
    }
    if (result == 0) {
        struct sockaddr_in any;
        memset(&any, 0, sizeof any);
        any.sin_family = AF_INET;
        any.sin_port = htons(68);
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (const struct sockaddr *)&any, sizeof any) != 0) {
            result = -errno;
        }
    }
    if (result != 0) {
        (void)close(fd);
        return result;
    }
    return fd;
}

int inkwell_netif_dhcp_send(int fd, const uint8_t *bytes, size_t len) {
    if (fd < 0 || bytes == NULL) {
        return -EINVAL;
    }
    struct sockaddr_in servers;
    memset(&servers, 0, sizeof servers);
    servers.sin_family = AF_INET;
    servers.sin_port = htons(67);
    servers.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const ssize_t sent =
        sendto(fd, bytes, len, 0, (const struct sockaddr *)&servers, sizeof servers);
    return sent < 0 ? -errno : (size_t)sent == len ? 0 : -EIO;
}
