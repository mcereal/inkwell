#include "inkwell/net/mqtt.h"

#include "inkwell/base/text.h"
#include "mqtt_address.h"

#include <errno.h>
#include <string.h>

int inkwell_mqtt_client_init(struct inkwell_mqtt_client *proxy, struct inkwell_loop *loop) {
    if (proxy == NULL) {
        return -EINVAL;
    }
    memset(proxy, 0, sizeof *proxy);
    proxy->loop = loop;
    proxy->fd = -1;
    proxy->state = INKWELL_MQTT_CLIENT_OFF;
    return inkwell_resolve_init(&proxy->resolve, loop);
}

void inkwell_mqtt_client_shutdown(struct inkwell_mqtt_client *proxy) {
    if (proxy != NULL) {
        inkwell_resolve_shutdown(&proxy->resolve);
        proxy->state = INKWELL_MQTT_CLIENT_OFF;
    }
}

void inkwell_mqtt_client_set_ca_bundle(struct inkwell_mqtt_client *proxy, const char *path) {
    if (proxy == NULL) {
        return;
    }
    if (path == NULL || !inkwell_str_copy(proxy->ca_bundle, sizeof proxy->ca_bundle, path)) {
        proxy->ca_bundle[0] = '\0';
    }
}

int inkwell_mqtt_client_start(struct inkwell_mqtt_client *proxy,
                              const struct inkwell_mqtt_client_config *config,
                              inkwell_mqtt_client_message_fn on_message,
                              inkwell_mqtt_client_state_fn on_state, void *userdata,
                              uint64_t now_ms) {
    (void)on_message;
    (void)on_state;
    (void)userdata;
    (void)now_ms;
    if (proxy == NULL || config == NULL || config->address[0] == '\0' ||
        config->client_id[0] == '\0') {
        return -EINVAL;
    }
    char host[INKWELL_MQTT_CLIENT_ADDRESS_MAX];
    uint16_t port = 0U;
    if (inkwell_mqtt_target_split(config->address, host, sizeof host, &port) != 0) {
        return -EINVAL;
    }
    return -ENOTSUP;
}

void inkwell_mqtt_client_stop(struct inkwell_mqtt_client *proxy) {
    if (proxy != NULL) {
        proxy->state = INKWELL_MQTT_CLIENT_OFF;
    }
}

int inkwell_mqtt_client_subscribe(struct inkwell_mqtt_client *proxy, const char *filter) {
    if (proxy == NULL || filter == NULL) {
        return -EINVAL;
    }
    const size_t len = strlen(filter);
    if (len == 0U || len >= INKWELL_MQTT_CLIENT_FILTER_MAX) {
        return -EINVAL;
    }
    for (size_t i = 0U; i < proxy->filter_count; ++i) {
        if (strcmp(proxy->filters[i], filter) == 0) {
            return -EEXIST;
        }
    }
    if (proxy->filter_count >= INKWELL_MQTT_CLIENT_FILTERS_MAX) {
        return -ENOSPC;
    }
    memcpy(proxy->filters[proxy->filter_count], filter, len + 1U);
    proxy->filter_count++;
    return 0;
}

void inkwell_mqtt_client_clear_filters(struct inkwell_mqtt_client *proxy) {
    if (proxy != NULL) {
        proxy->filter_count = 0U;
        proxy->filter_sent = 0U;
        proxy->subscribe_id = 0U;
    }
}

int inkwell_mqtt_client_publish(struct inkwell_mqtt_client *proxy, const char *topic,
                                const uint8_t *payload, size_t len, bool retained) {
    (void)payload;
    (void)len;
    (void)retained;
    if (proxy == NULL || topic == NULL) {
        return -EINVAL;
    }
    proxy->stats.dropped++;
    return -ENOTCONN;
}

void inkwell_mqtt_client_tick(struct inkwell_mqtt_client *proxy, uint64_t now_ms) {
    if (proxy != NULL) {
        proxy->now_ms = now_ms;
    }
}

enum inkwell_mqtt_client_state inkwell_mqtt_client_state(const struct inkwell_mqtt_client *proxy) {
    return proxy != NULL ? proxy->state : INKWELL_MQTT_CLIENT_OFF;
}

bool inkwell_mqtt_client_is_ready(const struct inkwell_mqtt_client *proxy) {
    (void)proxy;
    return false;
}

struct inkwell_mqtt_client_stats
inkwell_mqtt_client_stats(const struct inkwell_mqtt_client *proxy) {
    const struct inkwell_mqtt_client_stats empty = {0};
    return proxy != NULL ? proxy->stats : empty;
}

struct inkwell_mqtt_client_failure
inkwell_mqtt_client_failure(const struct inkwell_mqtt_client *proxy) {
    const struct inkwell_mqtt_client_failure none = {0};
    return proxy != NULL ? proxy->failure : none;
}

const char *inkwell_mqtt_client_failure_name(const struct inkwell_mqtt_client_failure *failure) {
    if (failure == NULL) {
        return "none";
    }
    switch (failure->refusal) {
    case INKWELL_MQTT_REFUSAL_BAD_ADDRESS:
        return "bad-address";
    case INKWELL_MQTT_REFUSAL_NO_TLS:
        return "no-tls";
    case INKWELL_MQTT_REFUSAL_PROTOCOL:
        return "not-mqtt";
    case INKWELL_MQTT_REFUSAL_BAD_LOGIN:
        return "bad-login";
    case INKWELL_MQTT_REFUSAL_NOT_ALLOWED:
        return "not-allowed";
    case INKWELL_MQTT_REFUSAL_BROKER_BUSY:
        return "broker-busy";
    case INKWELL_MQTT_REFUSAL_OTHER:
        return "refused";
    case INKWELL_MQTT_REFUSAL_NONE:
    case INKWELL_MQTT_REFUSAL_COUNT:
    default:
        break;
    }
    if (failure->net.reason == INKWELL_NET_OK) {
        return "none";
    }
    return inkwell_net_reason_name(failure->net.reason);
}

const char *inkwell_mqtt_client_tls_error(const struct inkwell_mqtt_client *proxy) {
    (void)proxy;
    return "";
}

const char *inkwell_mqtt_client_address(const struct inkwell_mqtt_client *proxy) {
    return proxy != NULL ? proxy->config.address : "";
}

const char *inkwell_mqtt_client_host(const struct inkwell_mqtt_client *proxy) {
    return proxy != NULL ? proxy->host : "";
}
