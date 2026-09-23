#pragma once

#include <stddef.h>
#include <stdint.h>

/* Internal parser shared by the socket and unavailable MQTT backends. */
int inkwell_mqtt_target_split(const char *target, char *host, size_t host_len, uint16_t *port);
