#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

INKWELL_TEST_CASE(socket_open_nonblocking, unit) {
    inkwell_socket socket = 0;
    INKWELL_TEST_FAIL_IF(inkwell_socket_open(AF_INET, SOCK_DGRAM, 0, NULL) != -EINVAL,
                         "open should reject a null output");
    const int result = inkwell_socket_open(AF_INET, SOCK_DGRAM, 0, &socket);
    INKWELL_TEST_FAIL_IF(result != 0 || socket == INKWELL_SOCKET_INVALID,
                         "open should create an IPv4 socket");
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
#if defined(_WIN32)
    const int bound = bind((SOCKET)socket, (const struct sockaddr *)&address, sizeof address);
#else
    const int bound = bind((int)socket, (const struct sockaddr *)&address, sizeof address);
#endif
    INKWELL_TEST_FAIL_IF_CLEANUP(bound != 0, (void)inkwell_socket_close(socket),
                                 "bind to loopback should succeed");
    char byte = 0;
    const int received = inkwell_socket_recv(socket, &byte, 1);
    const int pending = inkwell_socket_pending_error(socket);
    const int closed = inkwell_socket_close(socket);
    INKWELL_TEST_FAIL_IF(received != -EAGAIN, "a new socket should not block on recv");
    INKWELL_TEST_FAIL_IF(pending != 0, "a new socket should have no pending error");
    INKWELL_TEST_FAIL_IF(closed != 0, "close should succeed");
    record_success(test_name);
}

INKWELL_TEST_CASE(socket_rejects_invalid_handle, unit) {
    const inkwell_socket invalid = INKWELL_SOCKET_INVALID;
    char byte = 0;
    INKWELL_TEST_FAIL_IF(inkwell_socket_close(invalid) != -EINVAL,
                         "close should reject an invalid socket");
    INKWELL_TEST_FAIL_IF(inkwell_socket_connect(invalid, &byte, 1) != -EINVAL,
                         "connect should reject an invalid socket");
    INKWELL_TEST_FAIL_IF(inkwell_socket_send(invalid, &byte, 1) != -EINVAL,
                         "send should reject an invalid socket");
    INKWELL_TEST_FAIL_IF(inkwell_socket_recv(invalid, &byte, 1) != -EINVAL,
                         "recv should reject an invalid socket");
    INKWELL_TEST_FAIL_IF(inkwell_socket_pending_error(invalid) != -EINVAL,
                         "SO_ERROR should reject an invalid socket");
    record_success(test_name);
}

INKWELL_TEST_CASE(socket_loopback_datagram, unit) {
    const char *failure = NULL;
    inkwell_socket sender = INKWELL_SOCKET_INVALID;
    inkwell_socket receiver = INKWELL_SOCKET_INVALID;
    if (inkwell_socket_open(AF_INET, SOCK_DGRAM, 0, &sender) != 0 ||
        inkwell_socket_open(AF_INET, SOCK_DGRAM, 0, &receiver) != 0) {
        failure = "open should create two UDP sockets";
        goto done;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if defined(_WIN32)
    int address_len = sizeof address;
    if (bind((SOCKET)receiver, (const struct sockaddr *)&address, address_len) != 0 ||
        getsockname((SOCKET)receiver, (struct sockaddr *)&address, &address_len) != 0) {
#else
    socklen_t address_len = sizeof address;
    if (bind((int)receiver, (const struct sockaddr *)&address, address_len) != 0 ||
        getsockname((int)receiver, (struct sockaddr *)&address, &address_len) != 0) {
#endif
        failure = "receiver should bind on loopback";
        goto done;
    }
    if (inkwell_socket_connect(sender, &address, address_len) != 0) {
        failure = "sender should connect to receiver";
        goto done;
    }
    const char payload[] = "mesh";
    if (inkwell_socket_send(sender, payload, sizeof payload) != (int)sizeof payload) {
        failure = "send should write the datagram";
        goto done;
    }
    char bytes[sizeof payload] = {0};
    int received = -EAGAIN;
    for (int attempt = 0; attempt < 10000 && received == -EAGAIN; ++attempt) {
        received = inkwell_socket_recv(receiver, bytes, sizeof bytes);
    }
    if (received != (int)sizeof payload || memcmp(bytes, payload, sizeof payload) != 0) {
        failure = "receiver should read the datagram";
    }
done:
    if (sender != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(sender);
    }
    if (receiver != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(receiver);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}
