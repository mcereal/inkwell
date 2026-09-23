#include "framework/inkwell_test.h"

#include "inkwell/net/stream.h"

#include <errno.h>
#include <string.h>

struct windows_stream_probe {
    struct inkwell_stream *stream;
    uint8_t bytes[16];
    size_t length;
    int pump_result;
    unsigned callbacks;
};

static void windows_stream_bytes(void *userdata, const uint8_t *bytes, size_t len) {
    struct windows_stream_probe *probe = (struct windows_stream_probe *)userdata;
    if (len <= sizeof probe->bytes - probe->length) {
        memcpy(probe->bytes + probe->length, bytes, len);
        probe->length += len;
    }
}

static int windows_stream_ready(int token, uint32_t events, void *userdata) {
    struct windows_stream_probe *probe = (struct windows_stream_probe *)userdata;
    (void)token;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        probe->callbacks++;
        probe->pump_result = inkwell_stream_pump(probe->stream);
    }
    return 0;
}

INKWELL_TEST_CASE(stream_windows_round_trips_on_native_socket, unit) {
    const char *failure = NULL;
    inkwell_socket listener = INKWELL_SOCKET_INVALID;
    inkwell_socket client = INKWELL_SOCKET_INVALID;
    SOCKET peer = INVALID_SOCKET;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_stream stream;
    bool stream_open = false;
    struct inkwell_stream_slot slots[2];
    uint8_t queue[32];
    struct windows_stream_probe probe = {.stream = &stream};

    if (inkwell_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) != 0) {
        failure = "listener should open";
        goto done;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_len = sizeof address;
    if (bind((SOCKET)listener, (struct sockaddr *)&address, address_len) != 0 ||
        listen((SOCKET)listener, 1) != 0 ||
        getsockname((SOCKET)listener, (struct sockaddr *)&address, &address_len) != 0) {
        failure = "listener should bind";
        goto done;
    }
    if (inkwell_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) != 0) {
        failure = "client should open";
        goto done;
    }
    const int connected = inkwell_socket_connect(client, &address, sizeof address);
    if (connected != 0 && connected != -EINPROGRESS) {
        failure = "client should start connecting";
        goto done;
    }
    peer = accept((SOCKET)listener, NULL, NULL);
    if (peer == INVALID_SOCKET || inkwell_loop_init(&loop) != 0) {
        failure = "peer and loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_stream_init(&stream, "test", slots, 2U, queue, 16U) != 0) {
        failure = "stream should initialize";
        goto done;
    }
    if (inkwell_stream_open(&stream, 1, INKWELL_STREAM_SOCKET, NULL, NULL, NULL) != -ENOTSUP) {
        failure = "int socket API should refuse on Windows";
        goto done;
    }
    inkwell_stream_set_sink(&stream, windows_stream_bytes, NULL, &probe);
    if (inkwell_stream_open_socket(&stream, client, &loop, windows_stream_ready, &probe) != 0) {
        failure = "native socket should register";
        goto done;
    }
    stream_open = true;
    client = INKWELL_SOCKET_INVALID; /* stream owns it now */

    const char inbound[] = "hello";
    if (send(peer, inbound, sizeof inbound, 0) != sizeof inbound) {
        failure = "peer should send";
        goto done;
    }
    for (unsigned turn = 0; turn < 20U && probe.callbacks == 0U; ++turn) {
        (void)inkwell_loop_run(&loop, 20);
    }
    if (probe.callbacks == 0U || probe.pump_result != sizeof inbound ||
        probe.length != sizeof inbound || memcmp(probe.bytes, inbound, sizeof inbound) != 0) {
        failure = "loop should deliver socket bytes through stream";
        goto done;
    }

    const uint8_t outbound[] = {1U, 2U, 3U};
    uint8_t received[sizeof outbound] = {0};
    if (inkwell_stream_send(&stream, outbound, sizeof outbound, 7U) != 0 ||
        recv(peer, (char *)received, sizeof received, 0) != sizeof received ||
        memcmp(received, outbound, sizeof outbound) != 0) {
        failure = "stream should send bytes through native socket";
    }
done:
    if (stream_open) {
        inkwell_stream_close(&stream);
    }
    if (client != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(client);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    if (peer != INVALID_SOCKET) {
        (void)closesocket(peer);
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
