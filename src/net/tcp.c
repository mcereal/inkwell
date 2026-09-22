#define _POSIX_C_SOURCE 200809L

#include "inkwell/net/tcp.h"

#include "inkwell/base/fd.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void tcp_drop_socket(struct inkwell_tcp_connector *connector) {
    if (connector->fd < 0) {
        return;
    }
    if (connector->fd_registered && connector->loop != NULL) {
        (void)inkwell_loop_remove_fd(connector->loop, connector->fd);
    }
    (void)close(connector->fd);
    connector->fd = -1;
    connector->fd_registered = false;
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
            .fd = -1,
            .failure = {.reason = reason, .detail = detail},
        };
        callback(userdata, &result);
    }
}

static void tcp_complete_success(struct inkwell_tcp_connector *connector) {
    inkwell_tcp_connect_done_fn const callback = connector->on_done;
    void *const userdata = connector->userdata;
    const int fd = connector->fd;
    if (connector->fd_registered && connector->loop != NULL) {
        (void)inkwell_loop_remove_fd(connector->loop, fd);
    }
    connector->fd = -1;
    connector->fd_registered = false;
    tcp_clear_attempt(connector);
    if (callback != NULL) {
        const struct inkwell_tcp_connect_result result = {
            .fd = fd,
            .failure = {.reason = INKWELL_NET_OK, .detail = 0},
        };
        callback(userdata, &result);
    } else {
        (void)close(fd);
    }
}

static void tcp_finish_connect(struct inkwell_tcp_connector *connector) {
    int error = 0;
    socklen_t error_len = (socklen_t)sizeof error;
    if (getsockopt(connector->fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        error = errno;
    }
    if (error != 0) {
        tcp_complete_failure(connector, inkwell_net_reason_from_errno(error), -error);
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

static void tcp_set_unsigned_option(int fd, int option, unsigned value) {
    if (value == 0U || value > (unsigned)INT_MAX) {
        return;
    }
    const int setting = (int)value;
    (void)setsockopt(fd, IPPROTO_TCP, option, &setting, sizeof setting);
}

static void tcp_configure_socket(int fd, const struct inkwell_tcp_connect_options *options) {
    const int enabled = 1;
    if (options->no_delay) {
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
    }
    if (!options->keepalive) {
        return;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof enabled);
#if defined(TCP_KEEPIDLE)
    tcp_set_unsigned_option(fd, TCP_KEEPIDLE, options->keepalive_idle_s);
#elif defined(TCP_KEEPALIVE)
    /* Darwin's name for the same idle interval. */
    tcp_set_unsigned_option(fd, TCP_KEEPALIVE, options->keepalive_idle_s);
#endif
#if defined(TCP_KEEPINTVL)
    tcp_set_unsigned_option(fd, TCP_KEEPINTVL, options->keepalive_interval_s);
#endif
#if defined(TCP_KEEPCNT)
    tcp_set_unsigned_option(fd, TCP_KEEPCNT, options->keepalive_count);
#endif
}

static int tcp_open(struct inkwell_tcp_connector *connector, const struct sockaddr_storage *address,
                    socklen_t address_len, bool start_deadline,
                    struct inkwell_net_failure *failure) {
    const int fd = inkwell_fd_socket(address->ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        if (failure != NULL) {
            failure->reason = inkwell_net_reason_from_errno(fd);
            failure->detail = fd;
        }
        return fd;
    }
    tcp_configure_socket(fd, &connector->options);

    const int connected = connect(fd, (const struct sockaddr *)address, address_len);
    if (connected < 0 && errno != EINPROGRESS) {
        const int error = errno;
        (void)close(fd);
        if (failure != NULL) {
            failure->reason = inkwell_net_reason_from_errno(error);
            failure->detail = -error;
        }
        return -error;
    }

    connector->fd = fd;
    connector->state = INKWELL_TCP_CONNECT_CONNECTING;
    connector->deadline_ms =
        start_deadline ? connector->now_ms + connector->options.timeout_ms : 0U;
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
    const int added =
        inkwell_loop_add_fd(connector->loop, fd, INKWELL_LOOP_OUT, tcp_on_fd, connector);
    if (added < 0) {
        tcp_drop_socket(connector);
        if (failure != NULL) {
            failure->reason = inkwell_net_reason_from_errno(added);
            failure->detail = added;
        }
        return added;
    }
    connector->fd_registered = true;
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
    connector->fd = -1;
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
