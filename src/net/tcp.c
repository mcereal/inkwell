#define _POSIX_C_SOURCE 200809L

#include "inkwell/net/tcp.h"

#include "inkwell/base/fd.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static void tcp_drop_socket(struct inkwell_tcp_connector *connector) {
    if (connector->socket == INKWELL_SOCKET_INVALID) {
        return;
    }
    if (connector->registration_token >= 0 && connector->loop != NULL) {
        (void)inkwell_loop_remove_fd(connector->loop, connector->registration_token);
    }
    (void)inkwell_socket_close(connector->socket);
    connector->socket = INKWELL_SOCKET_INVALID;
    connector->registration_token = -1;
}

static void tcp_clear_attempt(struct inkwell_tcp_connector *connector) {
    connector->state = INKWELL_TCP_CONNECT_IDLE;
    connector->deadline_ms = 0U;
    connector->on_done = NULL;
    connector->userdata = NULL;
}

static void tcp_complete_failure(struct inkwell_tcp_connector *connector,
                                 enum inkwell_net_reason reason, int detail) {
    inkwell_tcp_connect_done_fn const callback = connector->on_done;
    void *const userdata = connector->userdata;
    tcp_drop_socket(connector);
    tcp_clear_attempt(connector);
    if (callback != NULL) {
        const struct inkwell_tcp_connect_result result = {
            .socket = INKWELL_SOCKET_INVALID,
            .failure = {.reason = reason, .detail = detail},
        };
        callback(userdata, &result);
    }
}

static void tcp_complete_success(struct inkwell_tcp_connector *connector) {
    inkwell_tcp_connect_done_fn const callback = connector->on_done;
    void *const userdata = connector->userdata;
    const inkwell_socket socket = connector->socket;
    if (connector->registration_token >= 0 && connector->loop != NULL) {
        (void)inkwell_loop_remove_fd(connector->loop, connector->registration_token);
    }
    connector->socket = INKWELL_SOCKET_INVALID;
    connector->registration_token = -1;
    tcp_clear_attempt(connector);
    if (callback != NULL) {
        const struct inkwell_tcp_connect_result result = {
            .socket = socket,
            .failure = {.reason = INKWELL_NET_OK, .detail = 0},
        };
        callback(userdata, &result);
    } else {
        (void)inkwell_socket_close(socket);
    }
}

static void tcp_finish_connect(struct inkwell_tcp_connector *connector) {
    const int error = inkwell_socket_pending_error(connector->socket);
    if (error < 0) {
        tcp_complete_failure(connector, inkwell_net_reason_from_errno(error), error);
        return;
    }
    tcp_complete_success(connector);
}

static int tcp_on_fd(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct inkwell_tcp_connector *connector = (struct inkwell_tcp_connector *)userdata;
    if (connector == NULL || connector->state != INKWELL_TCP_CONNECT_CONNECTING) {
        return 0;
    }
    if ((events & (uint32_t)(INKWELL_LOOP_OUT | INKWELL_LOOP_ERR | INKWELL_LOOP_HUP)) != 0U) {
        tcp_finish_connect(connector);
    }
    return 0;
}

static void tcp_configure_socket(inkwell_socket socket,
                                 const struct inkwell_tcp_connect_options *options) {
    if (options->no_delay) {
        (void)inkwell_socket_set_option(socket, INKWELL_SOCKET_NO_DELAY, 1U);
    }
    if (!options->keepalive) {
        return;
    }
    (void)inkwell_socket_set_option(socket, INKWELL_SOCKET_KEEPALIVE, 1U);
    if (options->keepalive_idle_s > 0U && options->keepalive_idle_s <= (unsigned)INT_MAX) {
        (void)inkwell_socket_set_option(socket, INKWELL_SOCKET_KEEPALIVE_IDLE_S,
                                        options->keepalive_idle_s);
    }
    if (options->keepalive_interval_s > 0U && options->keepalive_interval_s <= (unsigned)INT_MAX) {
        (void)inkwell_socket_set_option(socket, INKWELL_SOCKET_KEEPALIVE_INTERVAL_S,
                                        options->keepalive_interval_s);
    }
    if (options->keepalive_count > 0U && options->keepalive_count <= (unsigned)INT_MAX) {
        (void)inkwell_socket_set_option(socket, INKWELL_SOCKET_KEEPALIVE_COUNT,
                                        options->keepalive_count);
    }
}

static int tcp_open(struct inkwell_tcp_connector *connector, const struct sockaddr_storage *address,
                    socklen_t address_len, bool start_deadline,
                    struct inkwell_net_failure *failure) {
    inkwell_socket socket = INKWELL_SOCKET_INVALID;
    const int opened = inkwell_socket_open(address->ss_family, SOCK_STREAM, 0, &socket);
    if (opened < 0) {
        if (failure != NULL) {
            failure->reason = inkwell_net_reason_from_errno(opened);
            failure->detail = opened;
        }
        return opened;
    }
    tcp_configure_socket(socket, &connector->options);

    connector->socket = socket;
    connector->state = INKWELL_TCP_CONNECT_CONNECTING;
    connector->deadline_ms =
        start_deadline ? connector->now_ms + connector->options.timeout_ms : 0U;
    /* Register before connect(): a backend may report completion only once, so a fast refusal
       could otherwise finish before the loop starts watching the socket. */
    if (connector->loop != NULL) {
        const int added = inkwell_loop_watch_socket(connector->loop, socket, INKWELL_LOOP_OUT,
                                                    tcp_on_fd, connector);
        if (added < 0) {
            tcp_drop_socket(connector);
            if (failure != NULL) {
                failure->reason = inkwell_net_reason_from_errno(added);
                failure->detail = added;
            }
            return added;
        }
        connector->registration_token = added;
    }

    const int connected = inkwell_socket_connect(socket, address, (size_t)address_len);
    if (connected < 0 && connected != -EINPROGRESS) {
        tcp_drop_socket(connector);
        if (failure != NULL) {
            failure->reason = inkwell_net_reason_from_errno(connected);
            failure->detail = connected;
        }
        return connected;
    }

    if (connected == 0) {
        tcp_finish_connect(connector);
        return 0;
    }
    if (connector->loop == NULL) {
        tcp_drop_socket(connector);
        if (failure != NULL) {
            failure->reason = INKWELL_NET_UNREACHABLE;
            failure->detail = -ENOTSUP;
        }
        return -ENOTSUP;
    }
    return 0;
}

static void tcp_on_resolved(void *userdata, const struct inkwell_resolve_result *result) {
    struct inkwell_tcp_connector *connector = (struct inkwell_tcp_connector *)userdata;
    if (connector == NULL || connector->state != INKWELL_TCP_CONNECT_RESOLVING) {
        return;
    }
    if (result->outcome != INKWELL_RESOLVE_OK) {
        tcp_complete_failure(connector, inkwell_net_reason_from_resolve(result->outcome),
                             result->error);
        return;
    }
    connector->state = INKWELL_TCP_CONNECT_IDLE;
    struct inkwell_net_failure failure = {0};
    /* The lookup's time is not part of the connect deadline. The next tick arms a fresh one at
       the caller's current clock after this resolver callback returns to the loop. */
    const int opened = tcp_open(connector, &result->address, result->address_len, false, &failure);
    if (opened < 0) {
        tcp_complete_failure(connector, failure.reason, failure.detail);
    }
}

int inkwell_tcp_connector_init(struct inkwell_tcp_connector *connector, struct inkwell_loop *loop) {
    if (connector == NULL) {
        return -EINVAL;
    }
    memset(connector, 0, sizeof *connector);
    connector->loop = loop;
    connector->socket = INKWELL_SOCKET_INVALID;
    connector->registration_token = -1;
    connector->state = INKWELL_TCP_CONNECT_IDLE;
    return inkwell_resolve_init(&connector->resolve, loop);
}

void inkwell_tcp_connector_shutdown(struct inkwell_tcp_connector *connector) {
    if (connector == NULL) {
        return;
    }
    inkwell_tcp_connector_cancel(connector);
    inkwell_resolve_shutdown(&connector->resolve);
    connector->loop = NULL;
}

bool inkwell_tcp_connector_busy(const struct inkwell_tcp_connector *connector) {
    return connector != NULL && connector->state != INKWELL_TCP_CONNECT_IDLE;
}

int inkwell_tcp_connector_start(struct inkwell_tcp_connector *connector, const char *host,
                                uint16_t port, const struct inkwell_tcp_connect_options *options,
                                inkwell_tcp_connect_done_fn on_done, void *userdata,
                                uint64_t now_ms, struct inkwell_net_failure *failure) {
    if (failure != NULL) {
        memset(failure, 0, sizeof *failure);
    }
    if (connector == NULL || host == NULL || host[0] == '\0' || port == 0U || options == NULL ||
        options->timeout_ms == 0U || on_done == NULL) {
        if (failure != NULL) {
            failure->reason = INKWELL_NET_BAD_ADDRESS;
        }
        return -EINVAL;
    }
    if (inkwell_tcp_connector_busy(connector)) {
        return -EBUSY;
    }

    connector->options = *options;
    connector->on_done = on_done;
    connector->userdata = userdata;
    connector->now_ms = now_ms;

    struct sockaddr_storage address;
    socklen_t address_len = 0;
    if (inkwell_resolve_literal(host, port, &address, &address_len)) {
        const int opened = tcp_open(connector, &address, address_len, true, failure);
        if (opened < 0) {
            tcp_clear_attempt(connector);
        }
        return opened;
    }

    const int started =
        inkwell_resolve_start(&connector->resolve, host, port, tcp_on_resolved, connector, now_ms);
    if (started < 0) {
        tcp_clear_attempt(connector);
        if (failure != NULL) {
            failure->reason = INKWELL_NET_LOOKUP_FAILED;
            failure->detail = started;
        }
        return started;
    }
    connector->state = INKWELL_TCP_CONNECT_RESOLVING;
    return 0;
}

void inkwell_tcp_connector_tick(struct inkwell_tcp_connector *connector, uint64_t now_ms) {
    if (connector == NULL) {
        return;
    }
    connector->now_ms = now_ms;
    inkwell_resolve_tick(&connector->resolve, now_ms);
    if (connector->state == INKWELL_TCP_CONNECT_CONNECTING) {
        if (connector->deadline_ms == 0U) {
            connector->deadline_ms = now_ms + connector->options.timeout_ms;
        } else if (now_ms >= connector->deadline_ms) {
            tcp_complete_failure(connector, INKWELL_NET_TIMED_OUT, 0);
        }
    }
}

void inkwell_tcp_connector_cancel(struct inkwell_tcp_connector *connector) {
    if (connector == NULL) {
        return;
    }
    inkwell_resolve_cancel(&connector->resolve);
    tcp_drop_socket(connector);
    tcp_clear_attempt(connector);
}
