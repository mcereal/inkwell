#pragma once

#include "inkwell/ble/central.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN 18U

bool inkwell_ble_hci_parameters_valid(const struct inkwell_ble_connection_parameters *parameters);
bool inkwell_ble_hci_parse_address(const char *text, uint8_t out[6]);
int inkwell_ble_hci_adapter_index(const char *adapter_path);
size_t
inkwell_ble_hci_encode_connection_update(uint16_t handle,
                                         const struct inkwell_ble_connection_parameters *parameters,
                                         uint8_t out[INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN]);
