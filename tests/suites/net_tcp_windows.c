#include "framework/inkwell_test.h"

#include "inkwell/net/tcp.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>

struct windows_tcp_probe {
    unsigned calls;
    inkwell_socket socket;
    struct inkwell_net_failure failure;
};

static void windows_tcp_done(void *userdata, const struct inkwell_tcp_connect_result *result) {
    struct windows_tcp_probe *probe = (struct windows_tcp_probe *)userdata;
    probe->calls++;
    probe->socket = result->socket;
    probe->failure = result->failure;
}

INKWELL_TEST_CASE(tcp_windows_connects_literal, unit) {
    const char *failure = NULL;
    inkwell_socket listener = INKWELL_SOCKET_INVALID;
    SOCKET peer = INVALID_SOCKET;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_tcp_connector connector;
    bool connector_open = false;
    struct windows_tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};

    if (inkwell_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) != 0) {
        failure = "loopback listener should open";
        goto done;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_len = sizeof address;
    if (bind((SOCKET)listener, (struct sockaddr *)&address, address_len) != 0 ||
        listen((SOCKET)listener, 1) != 0 ||
        getsockname((SOCKET)listener, (struct sockaddr *)&address, &address_len) != 0) {
        failure = "loopback listener should bind";
        goto done;
    }
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should initialize";
        goto done;
    }
    loop_open = true;
    if (inkwell_tcp_connector_init(&connector, &loop) != 0) {
        failure = "connector should initialize";
        goto done;
    }
    connector_open = true;
    const struct inkwell_tcp_connect_options options = {
        .timeout_ms = 1000U, .no_delay = true, .keepalive = true};
    if (inkwell_tcp_connector_start(&connector, "127.0.0.1", ntohs(address.sin_port), &options,
                                    windows_tcp_done, &probe, 0U, NULL) != 0) {
        failure = "numeric connect should start";
        goto done;
    }
    for (unsigned turn = 0; turn < 50U && probe.calls == 0U; ++turn) {
        (void)inkwell_loop_run(&loop, 20);
        inkwell_tcp_connector_tick(&connector, (uint64_t)(turn + 1U) * 20U);
    }
    if (probe.calls != 1U || probe.socket == INKWELL_SOCKET_INVALID ||
        inkwell_net_failed(&probe.failure)) {
        failure = "connector should hand off one connected socket";
        goto done;
    }
    peer = accept((SOCKET)listener, NULL, NULL);
    if (peer == INVALID_SOCKET) {
        failure = "listener should accept the connection";
        goto done;
    }
    int no_delay = 0;
    int keepalive = 0;
    int len = sizeof no_delay;
    if (getsockopt((SOCKET)probe.socket, IPPROTO_TCP, TCP_NODELAY, (char *)&no_delay, &len) != 0) {
        failure = "TCP no-delay should be readable";
        goto done;
    }
    len = sizeof keepalive;
    if (getsockopt((SOCKET)probe.socket, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepalive, &len) != 0 ||
        no_delay == 0 || keepalive == 0) {
        failure = "connector should apply socket options";
    }
done:
    if (peer != INVALID_SOCKET) {
        (void)closesocket(peer);
    }
    if (probe.socket != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(probe.socket);
    }
    if (connector_open) {
        inkwell_tcp_connector_shutdown(&connector);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    if (listener != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(listener);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

INKWELL_TEST_CASE(tcp_windows_refuses_named_host_without_blocking, unit) {
    struct inkwell_tcp_connector connector;
    INKWELL_TEST_FAIL_IF(inkwell_tcp_connector_init(&connector, NULL) != 0,
                         "connector should initialize");
    struct windows_tcp_probe probe = {.socket = INKWELL_SOCKET_INVALID};
    const struct inkwell_tcp_connect_options options = {.timeout_ms = 1000U};
    struct inkwell_net_failure failure;
    const int result = inkwell_tcp_connector_start(&connector, "example.invalid", 4403U, &options,
                                                   windows_tcp_done, &probe, 0U, &failure);
    inkwell_tcp_connector_shutdown(&connector);
    INKWELL_TEST_FAIL_IF(result != -ENOTSUP || probe.calls != 0U,
                         "hostname lookup should refuse without a callback");
    record_success(test_name);
}
