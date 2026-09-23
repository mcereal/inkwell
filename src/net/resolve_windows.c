#include "inkwell/net/resolve.h"

#include <errno.h>
#include <string.h>

/* Numeric addresses require no background lookup and work before any socket is opened. */
bool inkwell_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                             socklen_t *out_len) {
    if (host == NULL || host[0] == '\0' || out == NULL || out_len == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct in_addr v4;
    if (InetPtonA(AF_INET, host, &v4) == 1) {
        struct sockaddr_in *address = (struct sockaddr_in *)out;
        address->sin_family = AF_INET;
        address->sin_port = htons(port);
        address->sin_addr = v4;
        *out_len = (socklen_t)sizeof *address;
        return true;
    }

    struct in6_addr v6;
    if (InetPtonA(AF_INET6, host, &v6) == 1) {
        struct sockaddr_in6 *address = (struct sockaddr_in6 *)out;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(port);
        address->sin6_addr = v6;
        *out_len = (socklen_t)sizeof *address;
        return true;
    }
    return false;
}

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
    (void)resolve;
    return false;
}

bool inkwell_resolve_busy(const struct inkwell_resolve *resolve) {
    (void)resolve;
    return false;
}

int inkwell_resolve_start(struct inkwell_resolve *resolve, const char *host, uint16_t port,
                          inkwell_resolve_done_fn on_done, void *userdata, uint64_t now_ms) {
    (void)port;
    (void)userdata;
    (void)now_ms;
    if (resolve == NULL || host == NULL || host[0] == '\0' || on_done == NULL) {
        return -EINVAL;
    }
    /* A later Windows resolver will register its completion with the event loop. Until then,
       refuse hostname work rather than blocking the UI thread with getaddrinfo(). */
    return -ENOTSUP;
}

void inkwell_resolve_tick(struct inkwell_resolve *resolve, uint64_t now_ms) {
    (void)resolve;
    (void)now_ms;
}

void inkwell_resolve_cancel(struct inkwell_resolve *resolve) {
    (void)resolve;
}
