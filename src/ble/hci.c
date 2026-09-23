#include "hci.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

#define INKWELL_HCI_COMMAND_PACKET 0x01U
/* OGF 0x08 (LE controller) << 10 | OCF 0x0013. */
#define INKWELL_HCI_OP_LE_CONNECTION_UPDATE 0x2013U

bool inkwell_ble_hci_parameters_valid(const struct inkwell_ble_connection_parameters *parameters) {
    if (parameters == NULL) {
        return false;
    }
    /* The timeout must outlive (1 + latency) * max interval * 2. Compare its 10 ms units
       against the interval's 1.25 ms units without floating point. */
    return parameters->min_interval >= 6U && parameters->max_interval <= 3200U &&
           parameters->min_interval <= parameters->max_interval && parameters->latency <= 499U &&
           parameters->supervision_timeout >= 10U && parameters->supervision_timeout <= 3200U &&
           (uint32_t)parameters->supervision_timeout * 8U >
               (1U + (uint32_t)parameters->latency) * (uint32_t)parameters->max_interval * 2U;
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool inkwell_ble_hci_parse_address(const char *text, uint8_t out[6]) {
    if (text == NULL || out == NULL || strlen(text) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 6U; ++i) {
        const int hi = hex_value((unsigned char)text[i * 3U]);
        const int lo = hex_value((unsigned char)text[i * 3U + 1U]);
        if (hi < 0 || lo < 0 || (i < 5U && text[i * 3U + 2U] != ':')) {
            return false;
        }
        /* The kernel's bdaddr_t stores the least significant octet first. */
        out[5U - i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

int inkwell_ble_hci_adapter_index(const char *adapter_path) {
    static const char k_prefix[] = "/org/bluez/hci";
    if (adapter_path == NULL || strncmp(adapter_path, k_prefix, sizeof k_prefix - 1U) != 0 ||
        adapter_path[sizeof k_prefix - 1U] == '\0') {
        return -EINVAL;
    }
    int index = 0;
    for (const char *cursor = adapter_path + sizeof k_prefix - 1U; *cursor != '\0'; ++cursor) {
        if (!isdigit((unsigned char)*cursor) || index > 1000) {
            return -EINVAL;
        }
        index = index * 10 + (*cursor - '0');
    }
    return index;
}

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)(value >> 8U);
}

size_t
inkwell_ble_hci_encode_connection_update(uint16_t handle,
                                         const struct inkwell_ble_connection_parameters *parameters,
                                         uint8_t out[INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN]) {
    if (out == NULL || handle > 0x0EFFU || !inkwell_ble_hci_parameters_valid(parameters)) {
        return 0U;
    }
    out[0] = INKWELL_HCI_COMMAND_PACKET;
    put_le16(&out[1], INKWELL_HCI_OP_LE_CONNECTION_UPDATE);
    out[3] = 14U;
    put_le16(&out[4], handle);
    put_le16(&out[6], parameters->min_interval);
    put_le16(&out[8], parameters->max_interval);
    put_le16(&out[10], parameters->latency);
    put_le16(&out[12], parameters->supervision_timeout);
    put_le16(&out[14], 0U);
    put_le16(&out[16], 0U);
    return INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN;
}
