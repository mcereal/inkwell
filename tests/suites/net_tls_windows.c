#include "framework/inkwell_test.h"
#include "support/tls_identity.h"

#include "inkwell/net/fetch.h"
#include "inkwell/net/mqtt.h"
#include "inkwell/net/tls.h"
#include "inkwell/runtime/loop.h"

/* Nothing to test without Mbed TLS, where every session is refused before it touches a socket. */
#ifdef INKWELL_HAVE_TLS

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * TLS over a native Winsock socket, against a peer on the same thread.
 *
 * The POSIX suites stand their server up in a forked child, which Windows does not have. What
 * these hold does not need one: that a session's bytes reach the wire through inkwell_socket and
 * the Windows loop - where a SOCKET is not an int - for both things that speak TLS, a fetch and
 * an MQTT broker connection. So the peer is non-blocking and pumped between turns of the loop,
 * the way net_mqtt_windows.c pumps its broker, and the protocol is covered where it always was.
 */

struct tls_peer {
    inkwell_socket listener;
    SOCKET socket;
    uint16_t port;
    bool ready;
    bool refused;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_ssl_context ssl;
    bool ssl_open;
    /* The bundle a client is told to trust, written to a file because that is the one path a
       client reads a bundle from. */
    char bundle[MAX_PATH + 48];
    uint8_t in[4096];
    size_t in_len;
};

static int tls_peer_send(void *ctx, const unsigned char *buf, size_t len) {
    const int sent = send((SOCKET)(uintptr_t)ctx, (const char *)buf, (int)len, 0);
    if (sent >= 0) {
        return sent;
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                               : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_peer_recv(void *ctx, unsigned char *buf, size_t len) {
    const int got = recv((SOCKET)(uintptr_t)ctx, (char *)buf, (int)len, 0);
    if (got > 0) {
        return got;
    }
    if (got == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ
                                               : MBEDTLS_ERR_NET_RECV_FAILED;
}

/* The certificate is always the fixture's; `trusted` is what the client is told to trust. */
static bool tls_peer_open(struct tls_peer *peer, const char *trusted) {
    static unsigned opened = 0U;
    memset(peer, 0, sizeof *peer);
    peer->listener = INKWELL_SOCKET_INVALID;
    peer->socket = INVALID_SOCKET;
    mbedtls_ssl_config_init(&peer->conf);
    mbedtls_x509_crt_init(&peer->cert);
    mbedtls_pk_init(&peer->key);

    char dir[MAX_PATH];
    const DWORD dir_len = GetTempPathA(sizeof dir, dir);
    if (dir_len == 0U || dir_len >= sizeof dir) {
        return false;
    }
    (void)snprintf(peer->bundle, sizeof peer->bundle, "%sinkwell-tls-%lu-%u.pem", dir,
                   (unsigned long)GetCurrentProcessId(), ++opened);
    FILE *file = fopen(peer->bundle, "wb");
    if (file == NULL) {
        peer->bundle[0] = '\0';
        return false;
    }
    const bool written = fputs(trusted, file) >= 0;
    if (fclose(file) != 0 || !written) {
        return false;
    }

    const char *const cert_pem = tls_identity_cert_pem();
    const char *const key_pem = tls_identity_key_pem();
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_x509_crt_parse(&peer->cert, (const unsigned char *)cert_pem,
                               strlen(cert_pem) + 1U) != 0 ||
        mbedtls_pk_parse_key(&peer->key, (const unsigned char *)key_pem, strlen(key_pem) + 1U, NULL,
                             0U) != 0 ||
        mbedtls_ssl_config_defaults(&peer->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&peer->conf, &peer->cert, &peer->key) != 0) {
        return false;
    }
    mbedtls_ssl_conf_authmode(&peer->conf, MBEDTLS_SSL_VERIFY_NONE);

    if (inkwell_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, &peer->listener) != 0) {
        return false;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_len = sizeof address;
    if (bind((SOCKET)peer->listener, (struct sockaddr *)&address, address_len) != 0 ||
        listen((SOCKET)peer->listener, 1) != 0 ||
        getsockname((SOCKET)peer->listener, (struct sockaddr *)&address, &address_len) != 0) {
        return false;
    }
    peer->port = ntohs(address.sin_port);
    return true;
}

static void tls_peer_close(struct tls_peer *peer) {
    if (peer->ssl_open) {
        mbedtls_ssl_free(&peer->ssl);
        peer->ssl_open = false;
    }
    mbedtls_ssl_config_free(&peer->conf);
    mbedtls_x509_crt_free(&peer->cert);
    mbedtls_pk_free(&peer->key);
    if (peer->socket != INVALID_SOCKET) {
        (void)closesocket(peer->socket);
        peer->socket = INVALID_SOCKET;
    }
    if (peer->listener != INKWELL_SOCKET_INVALID) {
        (void)inkwell_socket_close(peer->listener);
        peer->listener = INKWELL_SOCKET_INVALID;
    }
    if (peer->bundle[0] != '\0') {
        (void)DeleteFileA(peer->bundle);
        peer->bundle[0] = '\0';
    }
}

/* Whatever the peer can do without waiting: accept, a handshake step, or the plaintext that has
   arrived. The client only moves when its loop turns, so nothing here may block. */
static void tls_peer_pump(struct tls_peer *peer) {
    if (peer->socket == INVALID_SOCKET) {
        peer->socket = accept((SOCKET)peer->listener, NULL, NULL);
        if (peer->socket == INVALID_SOCKET) {
            return;
        }
        u_long nonblocking = 1UL;
        (void)ioctlsocket(peer->socket, (long)FIONBIO, &nonblocking);
        mbedtls_ssl_init(&peer->ssl);
        peer->ssl_open = true;
        if (mbedtls_ssl_setup(&peer->ssl, &peer->conf) != 0) {
            peer->refused = true;
            return;
        }
        mbedtls_ssl_set_bio(&peer->ssl, (void *)(uintptr_t)peer->socket, tls_peer_send,
                            tls_peer_recv, NULL);
    }
    if (peer->refused) {
        return;
    }
    if (!peer->ready) {
        const int rc = mbedtls_ssl_handshake(&peer->ssl);
        if (rc == 0) {
            peer->ready = true;
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            peer->refused = true;
        }
        return;
    }
    while (peer->in_len < sizeof peer->in) {
        const int got =
            mbedtls_ssl_read(&peer->ssl, peer->in + peer->in_len, sizeof peer->in - peer->in_len);
        if (got <= 0) {
            return;
        }
        peer->in_len += (size_t)got;
    }
}

/* Loopback takes a reply this size in one write; the bound is for a test that went wrong. */
static bool tls_peer_write(struct tls_peer *peer, const void *bytes, size_t len) {
    const unsigned char *at = (const unsigned char *)bytes;
    for (unsigned attempt = 0U; len > 0U && attempt < 1000U; ++attempt) {
        const int wrote = mbedtls_ssl_write(&peer->ssl, at, len);
        if (wrote > 0) {
            at += wrote;
            len -= (size_t)wrote;
        } else if (wrote != MBEDTLS_ERR_SSL_WANT_WRITE && wrote != MBEDTLS_ERR_SSL_WANT_READ) {
            return false;
        }
    }
    return len == 0U;
}

/* ---- fetch ---------------------------------------------------------------------------- */

struct fetch_probe {
    bool done;
    enum inkwell_fetch_outcome outcome;
    enum inkwell_net_reason reason;
    int status;
};

static void tls_fetch_done(void *userdata, const struct inkwell_fetch_result *result) {
    struct fetch_probe *probe = (struct fetch_probe *)userdata;
    probe->done = true;
    probe->outcome = result->outcome;
    probe->reason = result->failure.reason;
    probe->status = result->status;
}

/* A body with every byte a text-mode CRT would rewrite: LF, CRLF, ^Z and NUL. */
static const uint8_t k_body[] = {'M', 'Z', '\n', 'a', '\r', '\n', 0x1AU, 0x00U, 'z', '\n'};

INKWELL_TEST_CASE(tls_windows_fetch_writes_a_body_over_a_native_socket, unit) {
    const char *failure = NULL;
    struct tls_peer peer;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_fetch fetch;
    bool fetch_open = false;
    struct fetch_probe probe = {0};
    char output[MAX_PATH + 64] = "";
    uint64_t now_ms = 1000U;

    if (!tls_peer_open(&peer, tls_identity_cert_pem())) {
        failure = "peer should listen on loopback";
        goto done;
    }
    if (!inkwell_tls_available()) {
        failure = "this build should have TLS";
        goto done;
    }
    (void)snprintf(output, sizeof output, "%s.body", peer.bundle);
    (void)_putenv_s("SSL_CERT_FILE", peer.bundle);
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch should initialize";
        goto done;
    }
    fetch_open = true;
    if (!inkwell_fetch_available(&fetch)) {
        failure = "fetch should be available with TLS";
        goto done;
    }
    inkwell_fetch_connect_to(&fetch, "127.0.0.1", peer.port);
    const struct inkwell_fetch_request request = {.url = "https://example.invalid/image.bin",
                                                  .output_path = output,
                                                  .on_done = tls_fetch_done,
                                                  .userdata = &probe};
    if (inkwell_fetch_start(&fetch, &request, now_ms) != 0) {
        failure = "fetch should start";
        goto done;
    }

    bool replied = false;
    for (unsigned turn = 0U; turn < 500U && !probe.done; ++turn) {
        (void)inkwell_loop_run(&loop, 10);
        now_ms += 10U;
        inkwell_fetch_tick(&fetch, now_ms);
        tls_peer_pump(&peer);
        if (!replied && peer.ready && peer.in_len < sizeof peer.in) {
            peer.in[peer.in_len] = '\0';
            if (strstr((const char *)peer.in, "\r\n\r\n") != NULL) {
                char head[96];
                const int head_len =
                    snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
                             sizeof k_body);
                if (!tls_peer_write(&peer, head, (size_t)head_len) ||
                    !tls_peer_write(&peer, k_body, sizeof k_body)) {
                    failure = "peer should reply";
                    goto done;
                }
                (void)mbedtls_ssl_close_notify(&peer.ssl);
                replied = true;
            }
        }
    }
    if (!probe.done || probe.outcome != INKWELL_FETCH_OK || probe.status != 200) {
        failure = "fetch should finish with the peer's 200";
        goto done;
    }
    if (strncmp((const char *)peer.in, "GET /image.bin HTTP/1.1\r\n", 25U) != 0 ||
        strstr((const char *)peer.in, "\r\nHost: example.invalid\r\n") == NULL) {
        failure = "peer should read the request the client sent";
        goto done;
    }
    uint8_t written[64];
    FILE *file = fopen(output, "rb");
    const size_t written_len = file != NULL ? fread(written, 1U, sizeof written, file) : 0U;
    if (file != NULL) {
        (void)fclose(file);
    }
    if (written_len != sizeof k_body || memcmp(written, k_body, sizeof k_body) != 0) {
        failure = "the file should hold the body byte for byte";
    }

done:
    if (fetch_open) {
        inkwell_fetch_shutdown(&fetch);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    (void)_putenv_s("SSL_CERT_FILE", "");
    if (output[0] != '\0') {
        (void)DeleteFileA(output);
    }
    tls_peer_close(&peer);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

INKWELL_TEST_CASE(tls_windows_fetch_refuses_an_untrusted_peer, unit) {
    const char *failure = NULL;
    struct tls_peer peer;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_fetch fetch;
    bool fetch_open = false;
    struct fetch_probe probe = {0};
    uint64_t now_ms = 1000U;

    if (!tls_peer_open(&peer, tls_identity_decoy_pem())) {
        failure = "peer should listen on loopback";
        goto done;
    }
    (void)_putenv_s("SSL_CERT_FILE", peer.bundle);
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop should open";
        goto done;
    }
    loop_open = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch should initialize";
        goto done;
    }
    fetch_open = true;
    inkwell_fetch_connect_to(&fetch, "127.0.0.1", peer.port);
    const struct inkwell_fetch_request request = {
        .url = "https://example.invalid/", .on_done = tls_fetch_done, .userdata = &probe};
    if (inkwell_fetch_start(&fetch, &request, now_ms) != 0) {
        failure = "fetch should start";
        goto done;
    }
    for (unsigned turn = 0U; turn < 500U && !probe.done; ++turn) {
        (void)inkwell_loop_run(&loop, 10);
        now_ms += 10U;
        inkwell_fetch_tick(&fetch, now_ms);
        tls_peer_pump(&peer);
    }
    if (!probe.done || probe.outcome != INKWELL_FETCH_TLS || probe.reason != INKWELL_NET_TLS) {
        failure = "a peer the bundle does not vouch for should fail as TLS";
    }

done:
    if (fetch_open) {
        inkwell_fetch_shutdown(&fetch);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    (void)_putenv_s("SSL_CERT_FILE", "");
    tls_peer_close(&peer);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* ---- MQTT ----------------------------------------------------------------------------- */

struct mqtt_probe {
    unsigned messages;
    char topic[32];
};

static void tls_mqtt_message(void *userdata, const char *topic, const uint8_t *payload,
                             size_t len) {
    struct mqtt_probe *probe = (struct mqtt_probe *)userdata;
    (void)payload;
    (void)len;
    probe->messages++;
    (void)snprintf(probe->topic, sizeof probe->topic, "%s", topic);
}

static void tls_mqtt_turn(struct inkwell_loop *loop, struct inkwell_mqtt_client *client,
                          struct tls_peer *peer, uint64_t *now_ms) {
    (void)inkwell_loop_run(loop, 10);
    *now_ms += 10U;
    inkwell_mqtt_client_tick(client, *now_ms);
    tls_peer_pump(peer);
}

INKWELL_TEST_CASE(tls_windows_mqtt_session_runs_over_tls, unit) {
    const char *failure = NULL;
    struct tls_peer peer;
    struct inkwell_loop loop;
    bool loop_open = false;
    struct inkwell_mqtt_client client;
    bool client_open = false;
    struct mqtt_probe probe = {0};
    uint64_t now_ms = 1000U;

    if (!tls_peer_open(&peer, tls_identity_cert_pem())) {
        failure = "peer should listen on loopback";
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
    inkwell_mqtt_client_set_ca_bundle(&client, peer.bundle);
    struct inkwell_mqtt_client_config config;
    memset(&config, 0, sizeof config);
    /* By number: the certificate names 127.0.0.1 as an address, so it is checked as one. */
    (void)snprintf(config.address, sizeof config.address, "127.0.0.1:%u", (unsigned)peer.port);
    (void)snprintf(config.client_id, sizeof config.client_id, "%s", "windows-tls");
    config.tls_enabled = true;
    if (inkwell_mqtt_client_start(&client, &config, tls_mqtt_message, NULL, &probe, now_ms) != 0) {
        failure = "client should start";
        goto done;
    }

    /* CONNECT, decrypted: the whole packet is short enough for a one-byte remaining length. */
    for (unsigned turn = 0U; turn < 500U && (peer.in_len < 2U || peer.in_len < 2U + peer.in[1]);
         ++turn) {
        tls_mqtt_turn(&loop, &client, &peer, &now_ms);
    }
    if (!peer.ready || peer.in_len < 2U || peer.in[0] != 0x10U) {
        failure = "peer should read CONNECT through the session";
        goto done;
    }
    static const uint8_t connack[] = {0x20U, 0x02U, 0x00U, 0x00U};
    static const uint8_t publish[] = {0x30U, 0x08U, 0x00U, 0x03U, 'a', '/', 'b', 'h', 'i', '!'};
    if (!tls_peer_write(&peer, connack, sizeof connack) ||
        !tls_peer_write(&peer, publish, sizeof publish)) {
        failure = "peer should send CONNACK and PUBLISH";
        goto done;
    }
    for (unsigned turn = 0U; turn < 200U && probe.messages == 0U; ++turn) {
        tls_mqtt_turn(&loop, &client, &peer, &now_ms);
    }
    if (!inkwell_mqtt_client_is_ready(&client) || probe.messages != 1U ||
        strcmp(probe.topic, "a/b") != 0) {
        failure = "client should be ready and deliver the peer's PUBLISH";
    }

done:
    if (client_open) {
        inkwell_mqtt_client_shutdown(&client);
    }
    if (loop_open) {
        inkwell_loop_shutdown(&loop);
    }
    tls_peer_close(&peer);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

#endif /* INKWELL_HAVE_TLS */
