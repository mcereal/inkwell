#include "central_internal.h"

#include "inkwell/base/log.h"

#include <errno.h>

/*
 * The backend for a build with no Bluetooth stack under it: a Linux without the D-Bus headers,
 * or anything that is neither Linux nor macOS. open() refuses, so no other call here is ever
 * reached through central.c - they refuse too, rather than trusting that.
 */

const unsigned inkwell_ble_backend_write_timeout_ms = 3000U;
const unsigned inkwell_ble_backend_subscribe_timeout_ms = 3000U;

int inkwell_ble_backend_open(struct inkwell_ble_central *central, bool private_connection,
                             const char *bus_address) {
    (void)central;
    (void)private_connection;
    (void)bus_address;
    inkwell_log_debug("ble", "No Bluetooth stack in this build");
    return -ENOSYS;
}

void inkwell_ble_backend_close(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_check_ready(struct inkwell_ble_central *central) {
    (void)central;
    return -ENOSYS;
}

int inkwell_ble_backend_find_adapter(struct inkwell_ble_central *central, char *name,
                                     size_t name_len) {
    (void)central;
    (void)name;
    (void)name_len;
    return -ENOSYS;
}

int inkwell_ble_backend_discovery(struct inkwell_ble_central *central, bool on) {
    (void)central;
    (void)on;
    return -ENOSYS;
}

int inkwell_ble_backend_list_by_service(struct inkwell_ble_central *central,
                                        const char *service_uuid,
                                        struct inkwell_ble_device *devices, size_t capacity,
                                        size_t *count) {
    (void)central;
    (void)service_uuid;
    (void)devices;
    (void)capacity;
    (void)count;
    return -ENOSYS;
}

int inkwell_ble_backend_connect(struct inkwell_ble_central *central, const char *address,
                                uint32_t *token) {
    (void)central;
    (void)address;
    (void)token;
    return -ENOSYS;
}

int inkwell_ble_backend_pair(struct inkwell_ble_central *central, const char *address,
                             uint32_t *token) {
    (void)central;
    (void)address;
    (void)token;
    return -ENOSYS;
}

int inkwell_ble_backend_write(struct inkwell_ble_central *central, const char *handle,
                              const uint8_t *data, size_t len, uint32_t *token) {
    (void)central;
    (void)handle;
    (void)data;
    (void)len;
    (void)token;
    return -ENOSYS;
}

int inkwell_ble_backend_read(struct inkwell_ble_central *central, const char *handle,
                             uint32_t *token) {
    (void)central;
    (void)handle;
    (void)token;
    return -ENOSYS;
}

int inkwell_ble_backend_query(struct inkwell_ble_central *central, const char *address,
                              size_t which, uint32_t *token) {
    (void)central;
    (void)address;
    (void)which;
    (void)token;
    return -ENOSYS;
}

void inkwell_ble_backend_connect_cancel(struct inkwell_ble_central *central) {
    (void)central;
}

void inkwell_ble_backend_pair_cancel(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_disconnect(struct inkwell_ble_central *central, const char *address) {
    (void)central;
    (void)address;
    return -ENOSYS;
}

int inkwell_ble_backend_set_trusted(struct inkwell_ble_central *central, const char *address,
                                    bool trusted) {
    (void)central;
    (void)address;
    (void)trusted;
    return -ENOSYS;
}

int inkwell_ble_backend_forget(struct inkwell_ble_central *central, const char *address) {
    (void)central;
    (void)address;
    return -ENOSYS;
}

int inkwell_ble_backend_agent_register(struct inkwell_ble_central *central) {
    (void)central;
    return -ENOSYS;
}

void inkwell_ble_backend_agent_unregister(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_agent_answer(struct inkwell_ble_central *central, bool accept,
                                     uint32_t passkey) {
    (void)central;
    (void)accept;
    (void)passkey;
    return -ENOSYS;
}

int inkwell_ble_backend_find_characteristic(struct inkwell_ble_central *central,
                                            const char *address, const char *char_uuid,
                                            char *out_handle, size_t out_len) {
    (void)central;
    (void)address;
    (void)char_uuid;
    (void)out_handle;
    (void)out_len;
    return -ENOSYS;
}

int inkwell_ble_backend_mtu(struct inkwell_ble_central *central, const char *handle,
                            uint16_t *out_mtu) {
    (void)central;
    (void)handle;
    (void)out_mtu;
    return -ENOSYS;
}

int inkwell_ble_backend_subscribe(struct inkwell_ble_central *central, const char *handle,
                                  uint32_t *token) {
    (void)central;
    (void)handle;
    (void)token;
    return -ENOSYS;
}

int inkwell_ble_backend_attach(struct inkwell_ble_central *central) {
    (void)central;
    return 0;
}

void inkwell_ble_backend_detach(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_process(struct inkwell_ble_central *central) {
    (void)central;
    return 0;
}
