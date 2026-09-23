#include "inkwell/net/fetch.h"

#include "inkwell/base/text.h"
#include "inkwell/net/resolve.h"

#include <errno.h>
#include <string.h>

void inkwell_fetch_set_user_agent(const char *product) {
    (void)product;
}

int inkwell_fetch_init(struct inkwell_fetch *fetch, struct inkwell_loop *loop) {
    if (fetch == NULL) {
        return -EINVAL;
    }
    memset(fetch, 0, sizeof *fetch);
    fetch->loop = loop;
    return inkwell_resolve_init(&fetch->resolve, loop);
}

void inkwell_fetch_shutdown(struct inkwell_fetch *fetch) {
    if (fetch != NULL) {
        inkwell_resolve_shutdown(&fetch->resolve);
    }
}

bool inkwell_fetch_available(const struct inkwell_fetch *fetch) {
    (void)fetch;
    return false;
}

bool inkwell_fetch_busy(const struct inkwell_fetch *fetch) {
    (void)fetch;
    return false;
}

void inkwell_fetch_connect_to(struct inkwell_fetch *fetch, const char *host, uint16_t port) {
    if (fetch == NULL) {
        return;
    }
    if (host == NULL || !inkwell_str_copy(fetch->connect_host, sizeof fetch->connect_host, host)) {
        fetch->connect_host[0] = '\0';
    }
    fetch->connect_port = port;
}

int inkwell_fetch_start(struct inkwell_fetch *fetch, const struct inkwell_fetch_request *request,
                        uint64_t now_ms) {
    (void)now_ms;
    if (fetch == NULL || request == NULL || request->url == NULL || request->on_done == NULL) {
        return -EINVAL;
    }
    return -ENOTSUP;
}

void inkwell_fetch_tick(struct inkwell_fetch *fetch, uint64_t now_ms) {
    if (fetch != NULL) {
        fetch->now_ms = now_ms;
    }
}

void inkwell_fetch_cancel(struct inkwell_fetch *fetch) {
    (void)fetch;
}

/* Header inspection remains useful even when this host cannot make the request itself. */
bool inkwell_fetch_content_length(const char *headers, size_t len, uint64_t *out) {
    if (headers == NULL || out == NULL) {
        return false;
    }
    const size_t length = len > 0U ? len : strlen(headers);
    static const char k_key[] = "content-length:";
    const size_t key_len = sizeof k_key - 1U;
    for (size_t at = 0U; at + key_len <= length; ++at) {
        if (at != 0U && headers[at - 1U] != '\n') {
            continue;
        }
        bool matched = true;
        for (size_t i = 0U; i < key_len; ++i) {
            char c = headers[at + i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (c != k_key[i]) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        size_t digit = at + key_len;
        while (digit < length && (headers[digit] == ' ' || headers[digit] == '\t')) {
            digit++;
        }
        if (digit >= length || headers[digit] < '0' || headers[digit] > '9') {
            return false;
        }
        uint64_t parsed = 0U;
        while (digit < length && headers[digit] >= '0' && headers[digit] <= '9') {
            if (parsed > (UINT64_MAX - 9U) / 10U) {
                return false;
            }
            parsed = parsed * 10U + (uint64_t)(headers[digit] - '0');
            digit++;
        }
        *out = parsed;
        return true;
    }
    return false;
}

const char *inkwell_fetch_outcome_name(enum inkwell_fetch_outcome outcome) {
    switch (outcome) {
    case INKWELL_FETCH_OK:
        return "ok";
    case INKWELL_FETCH_HTTP_STATUS:
        return "http status";
    case INKWELL_FETCH_TOO_LARGE:
        return "too large";
    case INKWELL_FETCH_NETWORK:
        return "network";
    case INKWELL_FETCH_TLS:
        return "tls";
    case INKWELL_FETCH_PROTOCOL:
        return "protocol";
    case INKWELL_FETCH_FILE:
        return "file";
    case INKWELL_FETCH_TIMED_OUT:
        return "timed out";
    case INKWELL_FETCH_OUTCOME_COUNT:
    default:
        return "unknown";
    }
}
