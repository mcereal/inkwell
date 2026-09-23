#define _POSIX_C_SOURCE 200809L

#include "framework/inkwell_test.h"

#include "inkwell/net/tcp.h"
#include "inkwell/runtime/loop.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct tcp_probe {
    unsigned calls;
    inkwell_socket socket;
    struct inkwell_net_failure failure;
};

static void tcp_probe_done(void *userdata, const struct inkwell_tcp_connect_result *result) {
    struct tcp_probe *probe = (struct tcp_probe *)userdata;
    probe->calls++;
    probe->socket = result->socket;
    probe->failure = result->failure;
}

static int tcp_listener(uint16_t *port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 1) != 0) {
        (void)close(fd);
        return -1;
    }
    socklen_t len = (socklen_t)sizeof address;
    if (getsockname(fd, (struct sockaddr *)&address, &len) != 0) {
        (void)close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int tcp_listener6(uint16_t port) {
    const int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int enabled = 1;
    (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof enabled);
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof address);
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    address.sin6_port = htons(port);
    if (bind(fd, (const struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 1) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static bool tcp_wait(struct inkwell_loop *loop, struct inkwell_tcp_connector *connector,
                     struct tcp_probe *probe) {
    for (unsigned turn = 0U; turn < 100U && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(loop, 20);
        inkwell_tcp_connector_tick(connector, (uint64_t)(turn + 1U) * 20U);
    }
    return probe->calls != 0U;
}

INKWELL_TEST_CASE(tcp_connector_connects_a_literal, unit) {
    uint16_t port = 0U;
    const int listener = tcp_listener(&port);
    INKWELL_TEST_FAIL_IF(listener < 0, "the loopback listener did not start");

    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        (void)close(listener);
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct inkwell_tcp_connector connector;
    (void)inkwell_tcp_connector_init(&connector, &loop);
    struct tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};
    const struct inkwell_tcp_connect_options options = {
        .timeout_ms = 1000U,
        .no_delay = true,
        .keepalive = true,
        .keepalive_idle_s = 30U,
    };
    struct inkwell_net_failure failure;

    if (inkwell_tcp_connector_start(&connector, "127.0.0.1", port, &options, tcp_probe_done, &probe,
                                    0U, &failure) != 0 ||
        (!tcp_wait(&loop, &connector, &probe)) || probe.calls != 1U ||
        probe.socket == INKWELL_SOCKET_INVALID || inkwell_net_failed(&probe.failure)) {
        record_failure(test_name, "the connector did not return a connected descriptor");
        goto cleanup;
    }
    int keepalive = 0;
    socklen_t keepalive_len = (socklen_t)sizeof keepalive;
    if (getsockopt((int)probe.socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &keepalive_len) != 0 ||
        keepalive == 0) {
        record_failure(test_name, "the connected socket did not enable keepalive");
        goto cleanup;
    }
#if defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE)
    int idle = 0;
    socklen_t idle_len = (socklen_t)sizeof idle;
#if defined(TCP_KEEPIDLE)
    const int idle_option = TCP_KEEPIDLE;
#else
    const int idle_option = TCP_KEEPALIVE;
#endif
    if (getsockopt((int)probe.socket, IPPROTO_TCP, idle_option, &idle, &idle_len) != 0 ||
        idle != 30) {
        record_failure(test_name,
                       "the connected socket did not use the requested keepalive idle interval");
        goto cleanup;
    }
#endif
    const int accepted = accept(listener, NULL, NULL);
    if (accepted < 0) {
        record_failure(test_name, "the listener did not receive the connection");
        goto cleanup;
    }
    (void)close(accepted);
    record_success(test_name);

cleanup:
    if (probe.socket != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(probe.socket);
    }
    inkwell_tcp_connector_shutdown(&connector);
    inkwell_loop_shutdown(&loop);
    (void)close(listener);
}

INKWELL_TEST_CASE(tcp_connector_resolves_a_name, unit) {
    uint16_t port = 0U;
    const int listener = tcp_listener(&port);
    INKWELL_TEST_FAIL_IF(listener < 0, "the loopback listener did not start");
    /* `localhost` may resolve to either family first. Listen on both so this remains a test of
       the connector rather than of the machine's /etc/hosts ordering. */
    const int listener6 = tcp_listener6(port);
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        if (listener6 >= 0) {
            (void)close(listener6);
        }
        (void)close(listener);
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct inkwell_tcp_connector connector;
    (void)inkwell_tcp_connector_init(&connector, &loop);
    struct tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};
    const struct inkwell_tcp_connect_options options = {.timeout_ms = 1000U};

    if (inkwell_tcp_connector_start(&connector, "localhost", port, &options, tcp_probe_done, &probe,
                                    0U, NULL) != 0 ||
        !inkwell_tcp_connector_busy(&connector) || !tcp_wait(&loop, &connector, &probe)) {
        record_failure(test_name, "the named connection did not finish");
        goto cleanup;
    }
    if (probe.socket == INKWELL_SOCKET_INVALID || inkwell_net_failed(&probe.failure)) {
        record_failure(test_name, "localhost did not produce a connected descriptor");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    if (probe.socket != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(probe.socket);
    }
    inkwell_tcp_connector_shutdown(&connector);
    inkwell_loop_shutdown(&loop);
    if (listener6 >= 0) {
        (void)close(listener6);
    }
    (void)close(listener);
}

INKWELL_TEST_CASE(tcp_connector_cancel_suppresses_completion, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "the loop did not start");
    struct inkwell_tcp_connector connector;
    (void)inkwell_tcp_connector_init(&connector, &loop);
    struct tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};
    const struct inkwell_tcp_connect_options options = {.timeout_ms = 1000U};

    if (inkwell_tcp_connector_start(&connector, "inkwell-no-such-name.invalid", 4403U, &options,
                                    tcp_probe_done, &probe, 0U, NULL) != 0) {
        record_failure(test_name, "the lookup did not start");
        goto cleanup;
    }
    inkwell_tcp_connector_cancel(&connector);
    (void)inkwell_loop_run(&loop, 20);
    inkwell_tcp_connector_tick(&connector, 20U);
    if (probe.calls != 0U || inkwell_tcp_connector_busy(&connector)) {
        record_failure(test_name, "a cancelled attempt should be silent and idle");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    inkwell_tcp_connector_shutdown(&connector);
    inkwell_loop_shutdown(&loop);
}

INKWELL_TEST_CASE(tcp_connector_enforces_its_deadline, unit) {
    struct inkwell_tcp_connector connector;
    (void)inkwell_tcp_connector_init(&connector, NULL);
    struct tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};
    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        record_failure(test_name, "the socket pair did not open");
        goto cleanup;
    }

    /* Stand in for a socket whose connect has not reported yet. The deadline path only owns the
       pending descriptor and completion contract; how that descriptor became pending is covered
       by the loopback cases above. */
    connector.state = INKWELL_TCP_CONNECT_CONNECTING;
    connector.socket = (inkwell_socket)pair[0];
    connector.options.timeout_ms = 50U;
    connector.on_done = tcp_probe_done;
    connector.userdata = &probe;
    pair[0] = -1;

    inkwell_tcp_connector_tick(&connector, 100U);
    inkwell_tcp_connector_tick(&connector, 149U);
    if (probe.calls != 0U || !inkwell_tcp_connector_busy(&connector)) {
        record_failure(test_name, "the connector expired before its deadline");
        goto cleanup;
    }
    inkwell_tcp_connector_tick(&connector, 150U);
    if (probe.calls != 1U || probe.socket != INKWELL_SOCKET_INVALID ||
        probe.failure.reason != INKWELL_NET_TIMED_OUT || inkwell_tcp_connector_busy(&connector)) {
        record_failure(test_name, "the deadline did not report one timeout and return to idle");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    if (pair[0] >= 0) {
        (void)close(pair[0]);
    }
    if (pair[1] >= 0) {
        (void)close(pair[1]);
    }
    inkwell_tcp_connector_shutdown(&connector);
}
