#include "framework/inkwell_test.h"

#include "inkwell/net/mqtt.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The MQTT client over a native Winsock socket, against a broker small enough to read: one
 * loopback listener, one accepted peer, and the few packets a session needs. The POSIX suite in
 * net_mqtt.c covers the protocol itself; what these hold is that the same client reaches the
 * wire through inkwell_socket and the Windows loop, where a SOCKET is not an int.
 */

struct windows_broker {
    inkwell_socket listener;
    SOCKET peer;
    uint16_t port;
    uint8_t in[512];
    size_t in_len;
};

struct windows_mqtt_probe {
    unsigned messages;
    char topic[64];
    uint8_t payload[32];
    size_t payload_len;
};

static void windows_mqtt_message(void *userdata, const char *topic, const uint8_t *payload,
                                 size_t len) {
    struct windows_mqtt_probe *probe = (struct windows_mqtt_probe *)userdata;
    probe->messages++;
    (void)snprintf(probe->topic, sizeof probe->topic, "%s", topic);
    probe->payload_len = len < sizeof probe->payload ? len : sizeof probe->payload;
    if (probe->payload_len > 0U) {
        memcpy(probe->payload, payload, probe->payload_len);
    }
}

static bool windows_broker_listen(struct windows_broker *broker) {
    broker->listener = INKWELL_SOCKET_INVALID;
    broker->peer = INVALID_SOCKET;
    broker->in_len = 0U;
    if (inkwell_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, &broker->listener) != 0) {
        return false;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_len = sizeof address;
    if (bind((SOCKET)broker->listener, (struct sockaddr *)&address, address_len) != 0 ||
        listen((SOCKET)broker->listener, 1) != 0 ||
        getsockname((SOCKET)broker->listener, (struct sockaddr *)&address, &address_len) != 0) {
        return false;
    }
    broker->port = ntohs(address.sin_port);
    return true;
}

static void windows_broker_close(struct windows_broker *broker) {
    if (broker->peer != INVALID_SOCKET) {
        (void)closesocket(broker->peer);
        broker->peer = INVALID_SOCKET;
    }
    if (broker->listener != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(broker->listener);
        broker->listener = INKWELL_SOCKET_INVALID;
    }
}

/* One turn of everything the client needs to make progress. */
static void windows_mqtt_turn(struct inkwell_loop *loop, struct inkwell_mqtt_client *client,
                              uint64_t *now_ms) {
    (void)inkwell_loop_run(loop, 10);
    *now_ms += 10U;
    inkwell_mqtt_client_tick(client, *now_ms);
}

/* Accepts the client's connection and reads one whole packet from it, turning the loop between
   attempts: the client only writes when the loop runs, so a blocking read would deadlock. */
static bool windows_broker_read(struct windows_broker *broker, struct inkwell_loop *loop,
                                struct inkwell_mqtt_client *client, uint64_t *now_ms,
                                uint8_t *type, uint8_t *body, size_t *body_len) {
    for (unsigned turn = 0U; turn < 200U; ++turn) {
        if (broker->peer == INVALID_SOCKET) {
            broker->peer = accept((SOCKET)broker->listener, NULL, NULL);
            if (broker->peer != INVALID_SOCKET) {
                u_long nonblocking = 1UL;
                (void)ioctlsocket(broker->peer, (long)FIONBIO, &nonblocking);
            }
        }
        if (broker->peer != INVALID_SOCKET && broker->in_len < sizeof broker->in) {
            const int got = recv(broker->peer, (char *)broker->in + broker->in_len,
                                 (int)(sizeof broker->in - broker->in_len), 0);
            if (got > 0) {
                broker->in_len += (size_t)got;
            }
        }
        /* Every packet this broker reads is short enough for a one-byte remaining length. */
        if (broker->in_len >= 2U && broker->in_len >= 2U + broker->in[1]) {
            const size_t whole = 2U + broker->in[1];
            *type = broker->in[0];
            *body_len = broker->in[1];
            memcpy(body, broker->in + 2U, *body_len);
            memmove(broker->in, broker->in + whole, broker->in_len - whole);
            broker->in_len -= whole;
            return true;
        }
        windows_mqtt_turn(loop, client, now_ms);
    }
    return false;
}

static bool windows_broker_send(struct windows_broker *broker, const uint8_t *bytes, size_t len) {
    return broker->peer != INVALID_SOCKET &&
           send(broker->peer, (const char *)bytes, (int)len, 0) == (int)len;
}

static void windows_mqtt_config(struct inkwell_mqtt_client_config *config, uint16_t port) {
    memset(config, 0, sizeof *config);
    (void)snprintf(config->address, sizeof config->address, "127.0.0.1:%u", (unsigned)port);
    (void)snprintf(config->client_id, sizeof config->client_id, "%s", "windows-test");
}

INKWELL_TEST_CASE(mqtt_windows_session_round_trips_on_native_socket, unit) {
    const char *failure = NULL;
    struct windows_broker broker;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_mqtt_client client;
    bool client_open = false;
    struct windows_mqtt_probe probe = {0};
    uint64_t now_ms = 1000U;
    uint8_t type = 0U;
    uint8_t body[256];
    size_t body_len = 0U;

    if (!windows_broker_listen(&broker)) {
        failure = "broker should listen on loopback";
        goto done;
    }
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_mqtt_client_init(&client, &loop) != 0) {
        failure = "client should initialize";
        goto done;
    }
    client_open = true;
    struct inkwell_mqtt_client_config config;
    windows_mqtt_config(&config, broker.port);
    if (inkwell_mqtt_client_start(&client, &config, windows_mqtt_message, NULL, &probe, now_ms) !=
        0) {
        failure = "client should start";
        goto done;
    }

    if (!windows_broker_read(&broker, &loop, &client, &now_ms, &type, body, &body_len) ||
        type != 0x10U) {
        failure = "broker should receive CONNECT";
        goto done;
    }
    static const uint8_t connack[] = {0x20U, 0x02U, 0x00U, 0x00U};
    if (!windows_broker_send(&broker, connack, sizeof connack)) {
        failure = "broker should send CONNACK";
        goto done;
    }
    for (unsigned turn = 0U; turn < 100U && !inkwell_mqtt_client_is_ready(&client); ++turn) {
        windows_mqtt_turn(&loop, &client, &now_ms);
    }
    if (!inkwell_mqtt_client_is_ready(&client)) {
        failure = "client should be ready after CONNACK";
        goto done;
    }

    if (inkwell_mqtt_client_subscribe(&client, "msh/#") != 0 ||
        !windows_broker_read(&broker, &loop, &client, &now_ms, &type, body, &body_len) ||
        type != 0x82U || body_len < 2U) {
        failure = "broker should receive SUBSCRIBE";
        goto done;
    }
    const uint8_t suback[] = {0x90U, 0x03U, body[0], body[1], 0x00U};
    static const uint8_t publish[] = {0x30U, 0x08U, 0x00U, 0x03U, 'a', '/', 'b', 'h', 'i', '!'};
    if (!windows_broker_send(&broker, suback, sizeof suback) ||
        !windows_broker_send(&broker, publish, sizeof publish)) {
        failure = "broker should send SUBACK and PUBLISH";
        goto done;
    }
    for (unsigned turn = 0U; turn < 100U && probe.messages == 0U; ++turn) {
        windows_mqtt_turn(&loop, &client, &now_ms);
    }
    if (probe.messages != 1U || strcmp(probe.topic, "a/b") != 0 || probe.payload_len != 3U ||
        memcmp(probe.payload, "hi!", 3U) != 0) {
        failure = "client should deliver the broker's PUBLISH";
        goto done;
    }

    static const uint8_t outbound[] = {'o', 'k'};
    if (inkwell_mqtt_client_publish(&client, "c/d", outbound, sizeof outbound, false) != 0 ||
        !windows_broker_read(&broker, &loop, &client, &now_ms, &type, body, &body_len) ||
        type != 0x30U || body_len != 7U || memcmp(body, "\x00\x03" "c/dok", 7U) != 0) {
        failure = "broker should receive the client's PUBLISH";
        goto done;
    }

done:
    if (client_open) {
        inkwell_mqtt_client_shutdown(&client);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    windows_broker_close(&broker);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

INKWELL_TEST_CASE(mqtt_windows_backs_off_from_a_closed_port, unit) {
    const char *failure = NULL;
    struct windows_broker broker;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_mqtt_client client;
    bool client_open = false;
    uint64_t now_ms = 1000U;

    /* A port that was just free: bind one, note it, and close it before connecting. */
    if (!windows_broker_listen(&broker)) {
        failure = "broker should listen on loopback";
        goto done;
    }
    const uint16_t port = broker.port;
    windows_broker_close(&broker);
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_mqtt_client_init(&client, &loop) != 0) {
        failure = "client should initialize";
        goto done;
    }
    client_open = true;
    struct inkwell_mqtt_client_config config;
    windows_mqtt_config(&config, port);
    if (inkwell_mqtt_client_start(&client, &config, NULL, NULL, NULL, now_ms) != 0) {
        failure = "client should start";
        goto done;
    }
    /* Windows retries a refused loopback SYN for about two seconds before it reports one. */
    for (unsigned turn = 0U;
         turn < 400U && inkwell_mqtt_client_state(&client) != INKWELL_MQTT_CLIENT_WAITING;
         ++turn) {
        (void)inkwell_loop_run(&loop, 10);
        inkwell_mqtt_client_tick(&client, now_ms);
    }
    const struct inkwell_mqtt_client_failure why = inkwell_mqtt_client_failure(&client);
    if (inkwell_mqtt_client_state(&client) != INKWELL_MQTT_CLIENT_WAITING ||
        !inkwell_net_failed(&why.net) || client.socket != INKWELL_SOCKET_INVALID) {
        failure = "a refused connection should close the socket and wait to retry";
    }

done:
    if (client_open) {
        inkwell_mqtt_client_shutdown(&client);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

INKWELL_TEST_CASE(mqtt_windows_refuses_tls_without_a_backend, unit) {
    const char *failure = NULL;
    struct windows_broker broker;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_mqtt_client client;
    bool client_open = false;
    uint64_t now_ms = 1000U;

    if (!windows_broker_listen(&broker)) {
        failure = "broker should listen on loopback";
        goto done;
    }
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_mqtt_client_init(&client, &loop) != 0) {
        failure = "client should initialize";
        goto done;
    }
    client_open = true;
    struct inkwell_mqtt_client_config config;
    windows_mqtt_config(&config, broker.port);
    config.tls_enabled = true;
    if (inkwell_mqtt_client_start(&client, &config, NULL, NULL, NULL, now_ms) != 0) {
        failure = "client should start";
        goto done;
    }
    for (unsigned turn = 0U;
         turn < 200U && inkwell_mqtt_client_state(&client) != INKWELL_MQTT_CLIENT_WAITING;
         ++turn) {
        if (broker.peer == INVALID_SOCKET) {
            broker.peer = accept((SOCKET)broker.listener, NULL, NULL);
        }
        windows_mqtt_turn(&loop, &client, &now_ms);
    }
    if (inkwell_mqtt_client_failure(&client).refusal != INKWELL_MQTT_REFUSAL_NO_TLS) {
        failure = "TLS should be refused by name, not attempted";
    }

done:
    if (client_open) {
        inkwell_mqtt_client_shutdown(&client);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    windows_broker_close(&broker);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}
