#include "inkwell/net/probe.h"

#include "inkwell/codec/http.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct inkwell_probe_conn {
    struct inkwell_http_url url;
    inkwell_socket socket;
    int token; /* the loop's watch on the socket, or -1 */
    char request[INKWELL_HTTP_URL_MAX + 512U];
    size_t request_len;
    size_t sent;
    struct inkwell_http_response response;
    uint64_t deadline_ms;
    inkwell_probe_done_fn done;
    void *userdata;
};

static void release(struct inkwell_probe *probe) {
    struct inkwell_probe_conn *c = probe->conn;
    if (c == NULL) {
        return;
    }
    inkwell_tcp_connector_cancel(&probe->connector);
    if (c->token >= 0) {
        (void)inkwell_loop_remove_fd(probe->loop, c->token);
    }
    if (c->socket != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(c->socket);
    }
    free(c);
    probe->conn = NULL;
}

/* The one way a request ends: the probe idle again, then the callback - which may start the next.
 */
static void finish(struct inkwell_probe *probe, enum inkwell_probe_outcome outcome,
                   struct inkwell_net_failure failure) {
    struct inkwell_probe_conn *c = probe->conn;
    char location[INKWELL_PROBE_LOCATION_MAX];
    location[0] = '\0';
    int status = 0;
    if (outcome == INKWELL_PROBE_ANSWERED) {
        status = c->response.status;
        const char *value = NULL;
        size_t len = 0U;
        if (inkwell_http_response_header(&c->response, "Location", &value, &len) &&
            len < INKWELL_HTTP_URL_MAX) {
            char raw[INKWELL_HTTP_URL_MAX];
            memcpy(raw, value, len);
            raw[len] = '\0';
            struct inkwell_http_url target;
            if (inkwell_http_url_resolve(&c->url, raw, &target)) {
                const bool bracket = strchr(target.host, ':') != NULL;
                char port[8] = "";
                if (target.port != (target.tls ? 443U : 80U)) {
                    snprintf(port, sizeof port, ":%u", (unsigned)target.port);
                }
                const int n = snprintf(location, sizeof location, "%s://%s%s%s%s%s",
                                       target.tls ? "https" : "http", bracket ? "[" : "",
                                       target.host, bracket ? "]" : "", port, target.target);
                if (n < 0 || (size_t)n >= sizeof location) {
                    location[0] = '\0';
                }
            }
        }
    }
    const inkwell_probe_done_fn done = c->done;
    void *userdata = c->userdata;
    release(probe);
    const struct inkwell_probe_result result = {
        .outcome = outcome, .status = status, .location = location, .failure = failure};
    done(userdata, &result);
}

static void fail(struct inkwell_probe *probe, enum inkwell_probe_outcome outcome,
                 enum inkwell_net_reason reason, int detail) {
    finish(probe, outcome, (struct inkwell_net_failure){.reason = reason, .detail = detail});
}

static void read_some(struct inkwell_probe *probe) {
    struct inkwell_probe_conn *c = probe->conn;
    uint8_t buf[2048];
    for (;;) {
        const int got = inkwell_socket_recv(c->socket, buf, sizeof buf);
        if (got == -EAGAIN || got == -EWOULDBLOCK || got == -EINTR) {
            return;
        }
        if (got < 0) {
            fail(probe, INKWELL_PROBE_NETWORK, inkwell_net_reason_from_errno(got), got);
            return;
        }
        if (got == 0) {
            if (inkwell_http_response_head_done(&c->response)) {
                finish(probe, INKWELL_PROBE_ANSWERED, (struct inkwell_net_failure){0});
            } else if (inkwell_http_response_failed(&c->response) || c->response.head_len > 0U) {
                fail(probe, INKWELL_PROBE_PROTOCOL, INKWELL_NET_OK, 0);
            } else {
                fail(probe, INKWELL_PROBE_NETWORK, INKWELL_NET_CLOSED, 0);
            }
            return;
        }
        size_t at = 0U;
        while (at < (size_t)got) {
            const uint8_t *body = NULL;
            size_t body_len = 0U;
            const size_t used = inkwell_http_response_feed(&c->response, buf + at, (size_t)got - at,
                                                           &body, &body_len);
            if (inkwell_http_response_failed(&c->response)) {
                fail(probe, INKWELL_PROBE_PROTOCOL, INKWELL_NET_OK, 0);
                return;
            }
            if (inkwell_http_response_head_done(&c->response)) {
                finish(probe, INKWELL_PROBE_ANSWERED, (struct inkwell_net_failure){0});
                return;
            }
            if (used == 0U) {
                break;
            }
            at += used;
        }
    }
}

static void write_some(struct inkwell_probe *probe) {
    struct inkwell_probe_conn *c = probe->conn;
    while (c->sent < c->request_len) {
        const int n =
            inkwell_socket_send(c->socket, c->request + c->sent, c->request_len - c->sent);
        if (n == -EAGAIN || n == -EWOULDBLOCK || n == -EINTR) {
            return;
        }
        if (n < 0) {
            fail(probe, INKWELL_PROBE_NETWORK, inkwell_net_reason_from_errno(n), n);
            return;
        }
        c->sent += (size_t)n;
    }
    (void)inkwell_loop_update_fd(probe->loop, c->token, INKWELL_LOOP_IN);
}

static int on_socket(int token, uint32_t events, void *userdata) {
    (void)token;
    struct inkwell_probe *probe = userdata;
    if (probe->conn != NULL && (events & INKWELL_LOOP_OUT) != 0U) {
        write_some(probe);
    }
    if (probe->conn != NULL && (events & (INKWELL_LOOP_IN | INKWELL_LOOP_HUP | INKWELL_LOOP_ERR))) {
        read_some(probe);
    }
    return 0;
}

static void on_connected(void *userdata, const struct inkwell_tcp_connect_result *result) {
    struct inkwell_probe *probe = userdata;
    struct inkwell_probe_conn *c = probe->conn;
    if (c == NULL) {
        if (result->socket != INKWELL_SOCKET_INVALID) {
            (void)inkwell_socket_close(result->socket);
        }
        return;
    }
    if (result->socket == INKWELL_SOCKET_INVALID) {
        finish(probe,
               result->failure.reason == INKWELL_NET_TIMED_OUT ||
                       result->failure.reason == INKWELL_NET_LOOKUP_TIMED_OUT
                   ? INKWELL_PROBE_TIMED_OUT
                   : INKWELL_PROBE_NETWORK,
               result->failure);
        return;
    }
    c->socket = result->socket;
    const int token = inkwell_loop_watch_socket(
        probe->loop, (uintptr_t)c->socket, INKWELL_LOOP_IN | INKWELL_LOOP_OUT, on_socket, probe);
    if (token < 0) {
        fail(probe, INKWELL_PROBE_NETWORK, INKWELL_NET_UNREACHABLE, token);
        return;
    }
    c->token = token;
}

int inkwell_probe_init(struct inkwell_probe *probe, struct inkwell_loop *loop) {
    memset(probe, 0, sizeof *probe);
    probe->loop = loop;
    return inkwell_tcp_connector_init(&probe->connector, loop);
}

void inkwell_probe_shutdown(struct inkwell_probe *probe) {
    release(probe);
    inkwell_tcp_connector_shutdown(&probe->connector);
}

bool inkwell_probe_busy(const struct inkwell_probe *probe) {
    return probe->conn != NULL;
}

int inkwell_probe_start(struct inkwell_probe *probe, const char *url, uint32_t timeout_ms,
                        inkwell_probe_done_fn done, void *userdata, uint64_t now_ms) {
    if (probe->loop == NULL) {
        return -ENOTSUP;
    }
    if (probe->conn != NULL) {
        return -EBUSY;
    }
    if (done == NULL || url == NULL) {
        return -EINVAL;
    }
    struct inkwell_probe_conn *c = calloc(1U, sizeof *c);
    if (c == NULL) {
        return -ENOMEM;
    }
    if (!inkwell_http_url_parse(url, &c->url) || c->url.tls) {
        free(c);
        return -EINVAL;
    }
    const int len = inkwell_http_request_format(c->request, sizeof c->request, INKWELL_HTTP_GET,
                                                &c->url, NULL, 0U);
    if (len < 0) {
        free(c);
        return -EINVAL;
    }
    c->request_len = (size_t)len;
    c->socket = INKWELL_SOCKET_INVALID;
    c->token = -1;
    inkwell_http_response_init(&c->response, false);
    if (timeout_ms == 0U) {
        timeout_ms = INKWELL_PROBE_TIMEOUT_MS;
    }
    c->deadline_ms = now_ms + timeout_ms;
    c->done = done;
    c->userdata = userdata;
    probe->conn = c;
    const struct inkwell_tcp_connect_options options = {.timeout_ms = timeout_ms, .no_delay = true};
    struct inkwell_net_failure failure;
    const int r = inkwell_tcp_connector_start(&probe->connector, c->url.host, c->url.port, &options,
                                              on_connected, probe, now_ms, &failure);
    if (r < 0) {
        /* Refused before any callback was owed: nothing started. A connect that completed and
           failed synchronously has already finished it. */
        if (probe->conn == c) {
            release(probe);
        }
        return r;
    }
    return 0;
}

void inkwell_probe_tick(struct inkwell_probe *probe, uint64_t now_ms) {
    if (probe->conn == NULL) {
        return;
    }
    inkwell_tcp_connector_tick(&probe->connector, now_ms);
    if (probe->conn != NULL && now_ms >= probe->conn->deadline_ms) {
        fail(probe, INKWELL_PROBE_TIMED_OUT, INKWELL_NET_TIMED_OUT, 0);
    }
}

void inkwell_probe_cancel(struct inkwell_probe *probe) {
    release(probe);
}

const char *inkwell_probe_outcome_name(enum inkwell_probe_outcome outcome) {
    static const char *const k_names[INKWELL_PROBE_OUTCOME_COUNT] = {
        [INKWELL_PROBE_ANSWERED] = "answered",
        [INKWELL_PROBE_NETWORK] = "network",
        [INKWELL_PROBE_PROTOCOL] = "protocol",
        [INKWELL_PROBE_TIMED_OUT] = "timed out",
    };
    return (unsigned)outcome < INKWELL_PROBE_OUTCOME_COUNT ? k_names[outcome] : "unknown";
}
