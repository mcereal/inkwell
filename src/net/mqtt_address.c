#include "mqtt_address.h"

#include "inkwell/net/mqtt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int inkwell_mqtt_target_split(const char *target, char *host, size_t host_len, uint16_t *port) {
    if (target == NULL || host == NULL || host_len == 0U || port == NULL) {
        return -EINVAL;
    }

    const size_t len = strlen(target);
    if (len == 0U || len >= INKWELL_MQTT_CLIENT_ADDRESS_MAX) {
        return -EINVAL;
    }

    const char *host_start = target;
    size_t host_chars = len;
    const char *port_text = NULL;
    if (target[0] == '[') {
        const char *close = strchr(target, ']');
        if (close == NULL || close == target + 1) {
            return -EINVAL;
        }
        host_start = target + 1;
        host_chars = (size_t)(close - host_start);
        if (close[1] == ':') {
            port_text = close + 2;
        } else if (close[1] != '\0') {
            return -EINVAL;
        }
    } else {
        const char *colon = strchr(target, ':');
        if (colon != NULL && strchr(colon + 1, ':') == NULL) {
            host_chars = (size_t)(colon - target);
            port_text = colon + 1;
        }
    }
    if (host_chars == 0U || host_chars >= host_len) {
        return -EINVAL;
    }

    uint16_t parsed_port = 0U;
    if (port_text != NULL) {
        if (port_text[0] == '\0') {
            return -EINVAL;
        }
        char *end = NULL;
        const unsigned long value = strtoul(port_text, &end, 10);
        if (end == NULL || *end != '\0' || value == 0UL || value > 65535UL) {
            return -EINVAL;
        }
        parsed_port = (uint16_t)value;
    }

    memcpy(host, host_start, host_chars);
    host[host_chars] = '\0';
    *port = parsed_port;
    return 0;
}
