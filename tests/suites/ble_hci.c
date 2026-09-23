#include "framework/inkwell_test.h"

#include "../../src/ble/hci.h"
#include "inkwell/ble/central.h"

#include <errno.h>
#include <string.h>

static const struct inkwell_ble_connection_parameters k_fast = {
    .min_interval = 6U,
    .max_interval = 6U,
    .latency = 0U,
    .supervision_timeout = 400U,
};

INKWELL_TEST_CASE(ble_hci_connection_update_golden, unit) {
    /* Core spec, Vol 4 Part E 7.8.18: command packet, opcode 0x2013, fourteen parameter bytes. */
    static const uint8_t k_expected[INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN] = {
        0x01, 0x13, 0x20, 0x0E, 0x40, 0x00, 0x06, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t packet[INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN];
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_encode_connection_update(0x0040U, &k_fast, packet) !=
                                 sizeof packet ||
                             memcmp(packet, k_expected, sizeof k_expected) != 0,
                         "LE Connection Update did not encode byte for byte");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_hci_refuses_parameters_the_controller_cannot_accept, unit) {
    uint8_t packet[INKWELL_BLE_HCI_CONNECTION_UPDATE_LEN];
    struct inkwell_ble_connection_parameters bad = k_fast;
    bad.min_interval = 5U;
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_encode_connection_update(1U, &bad, packet) != 0U,
                         "an interval below 7.5 ms was accepted");
    bad = k_fast;
    bad.min_interval = 12U;
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_encode_connection_update(1U, &bad, packet) != 0U,
                         "a minimum above the maximum was accepted");
    bad = k_fast;
    bad.max_interval = 3200U;
    bad.supervision_timeout = 50U;
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_encode_connection_update(1U, &bad, packet) != 0U,
                         "a supervision timeout too short for the interval was accepted");
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_encode_connection_update(0x0F00U, &k_fast, packet) != 0U,
                         "a handle outside the controller's twelve-bit range was accepted");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_hci_parses_bluez_names, unit) {
    uint8_t bdaddr[6];
    static const uint8_t k_reversed[6] = {0xD9, 0x0A, 0x9D, 0x9E, 0x13, 0x9C};
    INKWELL_TEST_FAIL_IF(!inkwell_ble_hci_parse_address("9C:13:9E:9D:0A:D9", bdaddr) ||
                             memcmp(bdaddr, k_reversed, sizeof bdaddr) != 0,
                         "the address was not converted to bdaddr_t order");
    INKWELL_TEST_FAIL_IF(!inkwell_ble_hci_parse_address("9c:13:9e:9d:0a:d9", bdaddr),
                         "lower-case address was refused");
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_parse_address("9C:13:9E:9D:0A", bdaddr) ||
                             inkwell_ble_hci_parse_address("9C-13-9E-9D-0A-D9", bdaddr) ||
                             inkwell_ble_hci_parse_address("9C:13:9E:9D:0A:DZ", bdaddr),
                         "a malformed address was accepted");
    INKWELL_TEST_FAIL_IF(inkwell_ble_hci_adapter_index("/org/bluez/hci0") != 0 ||
                             inkwell_ble_hci_adapter_index("/org/bluez/hci12") != 12 ||
                             inkwell_ble_hci_adapter_index("/org/bluez") != -EINVAL ||
                             inkwell_ble_hci_adapter_index("/org/bluez/hci0/dev_X") != -EINVAL,
                         "a BlueZ adapter path was parsed incorrectly");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_connection_interval_is_a_central_operation, unit) {
    unsigned calls = 0U;
    const struct inkwell_ble_mock_config config = {
        .request_connection_interval_result = -EPERM,
        .request_connection_interval_calls = &calls,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    const int result =
        inkwell_ble_request_connection_interval(&central, "9C:13:9E:9D:0A:D9", &k_fast);
    struct inkwell_ble_connection_parameters bad = k_fast;
    bad.min_interval = 5U;
    const int invalid =
        inkwell_ble_request_connection_interval(&central, "9C:13:9E:9D:0A:D9", &bad);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(result != -EPERM || calls != 1U,
                         "the request did not reach the selected stack exactly once");
    INKWELL_TEST_FAIL_IF(invalid != -EINVAL || calls != 1U,
                         "invalid parameters reached the selected stack");
    record_success(test_name);
}
