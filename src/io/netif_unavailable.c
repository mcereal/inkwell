#include "inkwell/io/netif.h"

#include <errno.h>

/* Network interfaces are Linux's rtnetlink here; elsewhere every call refuses. */

int inkwell_netif_links(struct inkwell_netif_link *out, size_t max) {
    (void)out;
    (void)max;
    return -ENOSYS;
}

int inkwell_netif_link_named(const char *name, struct inkwell_netif_link *out) {
    (void)name;
    (void)out;
    return -ENOSYS;
}

int inkwell_netif_set_up(int index, bool up) {
    (void)index;
    (void)up;
    return -ENOSYS;
}

int inkwell_netif_addresses(int index, struct inkwell_netif_address *out, size_t max) {
    (void)index;
    (void)out;
    (void)max;
    return -ENOSYS;
}

int inkwell_netif_address_add(int index, const uint8_t address[4], uint8_t prefix) {
    (void)index;
    (void)address;
    (void)prefix;
    return -ENOSYS;
}

int inkwell_netif_address_flush(int index) {
    (void)index;
    return -ENOSYS;
}

int inkwell_netif_route_default(int index, const uint8_t gateway[4]) {
    (void)index;
    (void)gateway;
    return -ENOSYS;
}

int inkwell_netif_monitor_open(struct inkwell_netif_monitor *monitor, struct inkwell_loop *loop,
                               inkwell_netif_callback callback, void *userdata) {
    (void)loop;
    (void)callback;
    (void)userdata;
    if (monitor != NULL) {
        monitor->loop = NULL;
        monitor->fd = -1;
    }
    return -ENOSYS;
}

void inkwell_netif_monitor_close(struct inkwell_netif_monitor *monitor) {
    (void)monitor;
}

int inkwell_netif_dhcp_socket(const char *name) {
    (void)name;
    return -ENOSYS;
}

int inkwell_netif_dhcp_send(int fd, const uint8_t *bytes, size_t len) {
    (void)fd;
    (void)bytes;
    (void)len;
    return -ENOSYS;
}
