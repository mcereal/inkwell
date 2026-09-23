#include "framework/inkwell_test.h"

#include "inkwell/runtime/loop.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <errno.h>
#include <stdint.h>

struct socket_observation {
    SOCKET socket;
    int calls;
    char byte;
    uint32_t events;
};

static int socket_on_read(int token, uint32_t events, void *userdata) {
    (void)token;
    struct socket_observation *seen = (struct socket_observation *)userdata;
    seen->events |= events;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        seen->calls++;
        (void)recv(seen->socket, &seen->byte, 1, 0);
    }
    return 0;
}

INKWELL_TEST_CASE(loop_windows_socket_read_and_remove, unit) {
    WSADATA data;
    INKWELL_TEST_FAIL_IF(WSAStartup(MAKEWORD(2, 2), &data) != 0, "Winsock did not start");

    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET peer = INVALID_SOCKET;
    struct inkwell_loop loop;
    bool loop_open = false;
    bool ok = false;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        goto done;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(listener, 1) != 0) {
        goto done;
    }
    int address_len = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
        goto done;
    }
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET ||
        connect(client, (struct sockaddr *)&address, sizeof address) != 0) {
        goto done;
    }
    peer = accept(listener, NULL, NULL);
    if (peer == INVALID_SOCKET || inkwell_loop_init(&loop) != 0) {
        goto done;
    }
    loop_open = true;
    struct socket_observation seen = {.socket = client};
    const int token =
        inkwell_loop_add_socket(&loop, (uintptr_t)client, INKWELL_LOOP_IN, socket_on_read, &seen);
    if (token < 0 || inkwell_loop_add_socket(&loop, (uintptr_t)client, INKWELL_LOOP_IN,
                                             socket_on_read, &seen) != -EEXIST) {
        goto done;
    }
    if (send(peer, "z", 1, 0) != 1 || inkwell_loop_run(&loop, 100) != 0 || seen.calls != 1 ||
        seen.byte != 'z') {
        goto done;
    }
    closesocket(peer);
    peer = INVALID_SOCKET;
    if (inkwell_loop_run(&loop, 100) != 0 || (seen.events & INKWELL_LOOP_HUP) == 0U) {
        goto done;
    }
    ok = inkwell_loop_update_fd(&loop, token, INKWELL_LOOP_OUT) == 0 &&
         inkwell_loop_remove_fd(&loop, token) == 0;

done:
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    if (peer != INVALID_SOCKET) {
        closesocket(peer);
    }
    if (client != INVALID_SOCKET) {
        closesocket(client);
    }
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    WSACleanup();
    INKWELL_TEST_FAIL_IF(!ok, "Winsock readiness was not dispatched and removed cleanly");
    record_success(test_name);
}
