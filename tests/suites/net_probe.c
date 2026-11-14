#define _POSIX_C_SOURCE 200809L

#include "framework/inkwell_test.h"

#include "inkwell/net/probe.h"
#include "inkwell/runtime/loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A server on loopback, played between the loop's turns: it takes the connection, reads the
   request to its blank line, then sends `reply` (NULL: closes without a word) and closes. */
struct probe_server {
    int listener;
    int peer;
    uint16_t port;
    char request[1024];
    size_t request_len;
    const char *reply;
    bool answered;
};

struct probe_seen {
    unsigned calls;
    enum inkwell_probe_outcome outcome;
    int status;
    char location[INKWELL_PROBE_LOCATION_MAX];
};

static void probe_done(void *userdata, const struct inkwell_probe_result *result) {
    struct probe_seen *seen = userdata;
    seen->calls++;
    seen->outcome = result->outcome;
    seen->status = result->status;
    snprintf(seen->location, sizeof seen->location, "%s", result->location);
}

static bool server_open(struct probe_server *s, const char *reply) {
    memset(s, 0, sizeof *s);
    s->peer = -1;
    s->reply = reply;
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listener < 0) {
        return false;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof address;
    if (bind(s->listener, (const struct sockaddr *)&address, sizeof address) != 0 ||
        listen(s->listener, 1) != 0 ||
        getsockname(s->listener, (struct sockaddr *)&address, &len) != 0) {
        (void)close(s->listener);
        return false;
    }
    (void)fcntl(s->listener, F_SETFL, O_NONBLOCK);
    s->port = ntohs(address.sin_port);
    return true;
}

static void server_turn(struct probe_server *s) {
    if (s->peer < 0) {
        s->peer = accept(s->listener, NULL, NULL);
        if (s->peer < 0) {
            return;
        }
        (void)fcntl(s->peer, F_SETFL, O_NONBLOCK);
    }
    if (s->answered) {
        return;
    }
    const ssize_t got =
        recv(s->peer, s->request + s->request_len, sizeof s->request - 1U - s->request_len, 0);
    if (got > 0) {
        s->request_len += (size_t)got;
        s->request[s->request_len] = '\0';
    }
    if (strstr(s->request, "\r\n\r\n") != NULL) {
        if (s->reply != NULL) {
            (void)send(s->peer, s->reply, strlen(s->reply), 0);
        }
        (void)close(s->peer);
        s->peer = -2; /* done; never accept again */
        s->answered = true;
    }
}

static void server_close(struct probe_server *s) {
    if (s->peer >= 0) {
        (void)close(s->peer);
    }
    (void)close(s->listener);
}

/* Probes `path` on the server; true once the callback came. */
static bool probe_run(struct probe_server *s, const char *path, struct probe_seen *seen) {
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0) {
        return false;
    }
    struct inkwell_probe probe;
    (void)inkwell_probe_init(&probe, &loop);
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)s->port, path);
    bool ok = inkwell_probe_start(&probe, url, 2000U, probe_done, seen, 0U) == 0;
    for (unsigned turn = 0U; ok && turn < 200U && seen->calls == 0U; ++turn) {
        (void)inkwell_loop_run(&loop, 10);
        server_turn(s);
        inkwell_probe_tick(&probe, (uint64_t)(turn + 1U) * 10U);
    }
    ok = ok && seen->calls == 1U && !inkwell_probe_busy(&probe);
    inkwell_probe_shutdown(&probe);
    inkwell_loop_shutdown(&loop);
    return ok;
}

INKWELL_TEST_CASE(probe_hears_a_204, unit) {
    struct probe_server s;
    INKWELL_TEST_FAIL_IF(!server_open(&s, "HTTP/1.1 204 No Content\r\n\r\n"), "no listener");
    struct probe_seen seen = {0};
    const bool ran = probe_run(&s, "/generate_204", &seen);
    const bool asked = strncmp(s.request, "GET /generate_204 HTTP/1.1\r\n", 28) == 0 &&
                       strstr(s.request, "\r\nHost: 127.0.0.1:") != NULL;
    server_close(&s);
    INKWELL_TEST_FAIL_IF(!ran, "the probe did not finish once");
    INKWELL_TEST_FAIL_IF(!asked, "the request was not a GET of the path");
    INKWELL_TEST_FAIL_IF(seen.outcome != INKWELL_PROBE_ANSWERED || seen.status != 204 ||
                             seen.location[0] != '\0',
                         "a 204 was not heard as one");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_hands_back_a_redirect_without_following_it, unit) {
    struct probe_server s;
    INKWELL_TEST_FAIL_IF(!server_open(&s, "HTTP/1.1 302 Found\r\n"
                                          "Location: /portal?from=probe\r\n"
                                          "Content-Length: 5\r\n\r\nhello"),
                         "no listener");
    struct probe_seen seen = {0};
    const bool ran = probe_run(&s, "/generate_204", &seen);
    char want[96];
    snprintf(want, sizeof want, "http://127.0.0.1:%u/portal?from=probe", (unsigned)s.port);
    server_close(&s);
    INKWELL_TEST_FAIL_IF(!ran, "the probe did not finish once");
    INKWELL_TEST_FAIL_IF(seen.outcome != INKWELL_PROBE_ANSWERED || seen.status != 302,
                         "the redirect's status was not handed back");
    INKWELL_TEST_FAIL_IF(strcmp(seen.location, want) != 0, "the Location was not made absolute");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_hears_a_page_in_place_of_a_204, unit) {
    struct probe_server s;
    INKWELL_TEST_FAIL_IF(!server_open(&s, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
                                          "<html>sign in</html>"),
                         "no listener");
    struct probe_seen seen = {0};
    const bool ran = probe_run(&s, "/", &seen);
    server_close(&s);
    INKWELL_TEST_FAIL_IF(!ran || seen.outcome != INKWELL_PROBE_ANSWERED || seen.status != 200,
                         "a 200 was not heard");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_says_protocol_for_what_is_not_http, unit) {
    struct probe_server s;
    INKWELL_TEST_FAIL_IF(!server_open(&s, "SSH-2.0-nope\r\n\r\n"), "no listener");
    struct probe_seen seen = {0};
    const bool ran = probe_run(&s, "/", &seen);
    server_close(&s);
    INKWELL_TEST_FAIL_IF(!ran || seen.outcome != INKWELL_PROBE_PROTOCOL,
                         "a reply that is not HTTP was not PROTOCOL");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_says_network_for_a_close_without_a_word, unit) {
    struct probe_server s;
    INKWELL_TEST_FAIL_IF(!server_open(&s, NULL), "no listener");
    struct probe_seen seen = {0};
    const bool ran = probe_run(&s, "/", &seen);
    server_close(&s);
    INKWELL_TEST_FAIL_IF(!ran || seen.outcome != INKWELL_PROBE_NETWORK,
                         "a silent close was not NETWORK");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_refuses_https_and_a_second_request, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "the loop did not start");
    struct inkwell_probe probe;
    (void)inkwell_probe_init(&probe, &loop);
    struct probe_seen seen = {0};
    const int https = inkwell_probe_start(&probe, "https://127.0.0.1/", 0U, probe_done, &seen, 0U);
    const int first = inkwell_probe_start(&probe, "http://127.0.0.1:9/", 0U, probe_done, &seen, 0U);
    const int second =
        inkwell_probe_start(&probe, "http://127.0.0.1:9/", 0U, probe_done, &seen, 0U);
    inkwell_probe_cancel(&probe);
    const bool idle = !inkwell_probe_busy(&probe);
    inkwell_probe_shutdown(&probe);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(https != -EINVAL, "an https URL was not refused");
    /* Port 9 on loopback refuses at once, which may finish the first synchronously. */
    INKWELL_TEST_FAIL_IF(first != 0 && first != -ECONNREFUSED, "the first probe was refused");
    INKWELL_TEST_FAIL_IF(first == 0 && seen.calls == 0U && second != -EBUSY,
                         "a second probe while one runs was not -EBUSY");
    INKWELL_TEST_FAIL_IF(!idle, "a cancelled probe was still busy");
    record_success(test_name);
}

INKWELL_TEST_CASE(probe_without_a_loop_is_unavailable, unit) {
    struct inkwell_probe probe;
    (void)inkwell_probe_init(&probe, NULL);
    struct probe_seen seen = {0};
    const int r = inkwell_probe_start(&probe, "http://127.0.0.1/", 0U, probe_done, &seen, 0U);
    inkwell_probe_shutdown(&probe);
    INKWELL_TEST_FAIL_IF(r != -ENOTSUP, "a probe with no loop was not -ENOTSUP");
    record_success(test_name);
}
