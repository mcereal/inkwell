/*
 * The central's own bookkeeping, against the mock: what a caller sees of a connect, a pair, a
 * read and a write that are in flight, and what the mock promises about the stack it stands in
 * for. A consumer's tests are written against these promises, so they are held here rather than
 * only by whichever consumer happened to notice one.
 *
 * Nothing here reaches a real Bluetooth stack. The BlueZ backend's D-Bus half has a test of its
 * own on an isolated bus (tests/ble_bluez_bus.c).
 */

#include "framework/inkwell_test.h"

#include "inkwell/ble/central.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <string.h>

#define SERVICE "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD"
#define OTHER_SERVICE "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define ADDRESS "AA:BB:CC:DD:EE:01"

static void count(void *userdata) {
    ++*(unsigned *)userdata;
}

struct received {
    unsigned calls;
    uint8_t last[8];
    size_t last_len;
};

static void on_notification(const uint8_t *data, size_t len, void *userdata) {
    struct received *received = userdata;
    received->calls++;
    received->last_len = len < sizeof received->last ? len : sizeof received->last;
    memcpy(received->last, data, received->last_len);
}

INKWELL_TEST_CASE(ble_list_filters_by_service_and_marks_range, unit) {
    const struct inkwell_ble_device devices[] = {
        {.address = ADDRESS, .name = "heard", .rssi = -60},
        {.address = "AA:BB:CC:DD:EE:02", .name = "bonded, away", .paired = true},
        {.address = "AA:BB:CC:DD:EE:03", .name = "other service", .rssi = -70},
    };
    const char *const services[] = {SERVICE, SERVICE, OTHER_SERVICE};
    unsigned listings = 0U;
    const struct inkwell_ble_mock_config config = {
        .devices = devices,
        .device_count = 3U,
        .device_service_uuids = services,
        .list_calls = &listings,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    const int opened = inkwell_ble_open(&central);

    struct inkwell_ble_device found[4];
    size_t n = 0U;
    /* Case does not matter: the stack spells a UUID however it likes. */
    const int listed = inkwell_ble_list_by_service(&central, "6ba1b218-15a8-461f-9fa8-5dcae273eafd",
                                                   found, 4U, &n);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(opened != 0 || listed != 0, "open or list failed");
    INKWELL_TEST_FAIL_IF(n != 2U, "the listing did not filter by service");
    INKWELL_TEST_FAIL_IF(!found[0].in_range, "a device with a signal strength was not in range");
    INKWELL_TEST_FAIL_IF(found[1].in_range, "a bond with no RSSI behind it was called in range");
    INKWELL_TEST_FAIL_IF(listings != 1U, "the listing was not counted");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_connect_is_polled_and_one_at_a_time, unit) {
    const struct inkwell_ble_mock_config config = {.connect_pending_polls = 2U};
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);

    const int begun = inkwell_ble_connect_begin(&central, ADDRESS);
    const int again = inkwell_ble_connect_begin(&central, ADDRESS);
    int result = 1;
    const int first = inkwell_ble_connect_poll(&central, &result);
    const int second = inkwell_ble_connect_poll(&central, &result);
    const int third = inkwell_ble_connect_poll(&central, &result);
    const int idle = inkwell_ble_connect_poll(&central, &result);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(begun != 0, "connect did not start");
    INKWELL_TEST_FAIL_IF(again != -EBUSY, "a second connect was allowed while one was in flight");
    INKWELL_TEST_FAIL_IF(first != 0 || second != 0, "the connect finished before its reply");
    INKWELL_TEST_FAIL_IF(third != 1 || result != 0, "the connect never finished");
    INKWELL_TEST_FAIL_IF(idle != -EINVAL, "a finished connect answered a poll twice");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_connect_after_the_scan_stops_can_miss, unit) {
    const struct inkwell_ble_mock_config config = {.connect_needs_the_scan = true};
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);

    int unscanned = 0;
    (void)inkwell_ble_connect_begin(&central, ADDRESS);
    (void)inkwell_ble_connect_poll(&central, &unscanned);

    (void)inkwell_ble_start_discovery(&central);
    int scanned = 1;
    (void)inkwell_ble_connect_begin(&central, ADDRESS);
    (void)inkwell_ble_connect_poll(&central, &scanned);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(unscanned != -ENOENT, "a device with no scan behind it was reachable");
    INKWELL_TEST_FAIL_IF(scanned != 0, "a device found by a live scan was not");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_pair_waits_for_the_passkey, unit) {
    uint32_t answered = 0U;
    const struct inkwell_ble_device device = {.address = ADDRESS, .rssi = -50};
    const struct inkwell_ble_mock_config config = {
        .pair_requests_passkey = true,
        .pair_passkey_capture = &answered,
        .devices = &device,
        .device_count = 1U,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    (void)inkwell_ble_agent_register(&central);

    (void)inkwell_ble_pair_begin(&central, ADDRESS);
    struct inkwell_ble_agent_request request;
    const bool asked = inkwell_ble_agent_request(&central, &request);
    int result = 1;
    const int before = inkwell_ble_pair_poll(&central, &result);
    const int submitted = inkwell_ble_agent_submit_passkey(&central, 123456U);
    const bool still_asking = inkwell_ble_agent_request(&central, NULL);
    const int after = inkwell_ble_pair_poll(&central, &result);

    struct inkwell_ble_device listed;
    size_t n = 0U;
    (void)inkwell_ble_list_by_service(&central, SERVICE, &listed, 1U, &n);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(!asked || request.kind != INKWELL_BLE_AGENT_REQUEST_PASSKEY ||
                             strcmp(request.address, ADDRESS) != 0,
                         "the pair did not ask for a passkey for the address it was begun on");
    INKWELL_TEST_FAIL_IF(before != 0, "the pair finished while the agent was waiting on the user");
    INKWELL_TEST_FAIL_IF(submitted != 0 || answered != 123456U || still_asking,
                         "the passkey did not reach the stack or the request stayed pending");
    INKWELL_TEST_FAIL_IF(after != 1 || result != 0, "the pair never finished once answered");
    INKWELL_TEST_FAIL_IF(n != 1U || !listed.paired, "a finished pair did not list as paired");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_rejecting_the_question_refuses_the_bond, unit) {
    const struct inkwell_ble_mock_config config = {.pair_requests_passkey = true};
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);

    (void)inkwell_ble_pair_begin(&central, ADDRESS);
    const int rejected = inkwell_ble_agent_reject(&central);
    const int nothing = inkwell_ble_agent_reject(&central);
    int result = 0;
    const int polled = inkwell_ble_pair_poll(&central, &result);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(rejected != 0 || nothing != -ENOENT, "reject did not clear the request");
    INKWELL_TEST_FAIL_IF(polled != 1 || result != -EACCES, "a refused pair did not fail -EACCES");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_notifications_follow_the_subscription, unit) {
    inkwell_ble_mock_enable(NULL);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    struct received received = {0};
    inkwell_ble_set_notification_handler(&central, on_notification, &received);

    (void)inkwell_ble_connect_begin(&central, ADDRESS);
    int result = 0;
    (void)inkwell_ble_connect_poll(&central, &result);
    char handle[INKWELL_BLE_HANDLE_MAX];
    const int found =
        inkwell_ble_find_characteristic(&central, ADDRESS, SERVICE, handle, sizeof handle);
    const int subscribed = inkwell_ble_subscribe(&central, handle);

    const uint8_t value[] = {1, 2, 3};
    inkwell_ble_mock_emit_notification("somewhere/else", value, sizeof value);
    const unsigned stray = received.calls;
    inkwell_ble_mock_emit_notification(handle, value, sizeof value);
    const unsigned delivered = received.calls;
    (void)inkwell_ble_disconnect(&central, ADDRESS);
    inkwell_ble_mock_emit_notification(handle, value, sizeof value);
    const unsigned after = received.calls;
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(found != 0 || strcmp(handle, ADDRESS "/" SERVICE) != 0,
                         "a mock handle is not <address>/<uuid>");
    INKWELL_TEST_FAIL_IF(subscribed != 0, "subscribe failed");
    INKWELL_TEST_FAIL_IF(stray != 0U, "a notification for another characteristic was delivered");
    INKWELL_TEST_FAIL_IF(delivered != 1U || received.last_len != 3U || received.last[2] != 3U,
                         "the subscribed notification was not delivered intact");
    INKWELL_TEST_FAIL_IF(after != 1U, "a notification arrived after the disconnect");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_read_waits_and_wakes, unit) {
    static const uint8_t first[] = {0x08, 0x01};
    const uint8_t *const payloads[] = {first};
    const size_t lengths[] = {sizeof first};
    const struct inkwell_ble_mock_config config = {
        .read_payloads = payloads,
        .read_payload_lengths = lengths,
        .read_payload_count = 1U,
        .read_pending_polls = 2U,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    unsigned woken = 0U;
    central.read_ready = count;
    central.userdata = &woken;

    uint8_t out[16];
    size_t len = 99U;
    const int started = inkwell_ble_read(&central, "h", out, sizeof out, &len);
    const int waiting = inkwell_ble_read(&central, "h", out, sizeof out, &len);
    (void)inkwell_ble_process(&central);
    const unsigned woken_early = woken;
    (void)inkwell_ble_process(&central);
    const int done = inkwell_ble_read(&central, "h", out, sizeof out, &len);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(started != -EAGAIN || waiting != -EAGAIN,
                         "a pending read did not answer -EAGAIN");
    INKWELL_TEST_FAIL_IF(woken_early != 0U || woken != 1U, "read_ready fired early or not at all");
    INKWELL_TEST_FAIL_IF(done != 0 || len != 2U || out[0] != 0x08U, "the read's value was lost");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_read_script_runs_dry_as_an_empty_value, unit) {
    static const uint8_t only[] = {0x2A};
    const uint8_t *const payloads[] = {only};
    const size_t lengths[] = {sizeof only};
    const struct inkwell_ble_mock_config config = {
        .read_payloads = payloads,
        .read_payload_lengths = lengths,
        .read_payload_count = 1U,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    uint8_t out[4];
    size_t len = 0U;
    const int first = inkwell_ble_read(&central, "h", out, sizeof out, &len);
    size_t empty_len = 99U;
    const int empty = inkwell_ble_read(&central, "h", out, sizeof out, &empty_len);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(first != 0 || len != 1U || out[0] != 0x2AU, "the scripted read was lost");
    INKWELL_TEST_FAIL_IF(empty != 0 || empty_len != 0U, "an exhausted script is not an empty read");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_write_is_captured_and_can_start_failing, unit) {
    uint8_t captured[8];
    size_t captured_len = 0U;
    char captured_handle[32];
    size_t writes = 0U;
    const struct inkwell_ble_mock_config config = {
        .write_capture_buffer = captured,
        .write_capture_capacity = sizeof captured,
        .write_capture_length = &captured_len,
        .write_capture_handle = captured_handle,
        .write_capture_handle_capacity = sizeof captured_handle,
        .write_call_count = &writes,
        .write_fail_after_calls = 1U,
        .write_result_late = -ENOTCONN,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);

    const uint8_t data[] = {7, 8, 9};
    const int first = inkwell_ble_write(&central, "to", data, sizeof data);
    const int second = inkwell_ble_write(&central, "to", data, sizeof data);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(first != 0 || second != -ENOTCONN, "write_fail_after_calls not honoured");
    INKWELL_TEST_FAIL_IF(writes != 2U || captured_len != 3U || captured[0] != 7U ||
                             strcmp(captured_handle, "to") != 0,
                         "the write was not captured");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_link_state_is_scripted, unit) {
    const struct inkwell_ble_mock_config config = {
        .services_resolved_timeout_polls = 1U,
        .services_resolved_after_polls = 1U,
        .connected_drops_after_polls = 1U,
    };
    inkwell_ble_mock_enable(&config);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);

    bool resolved = true;
    const int busy = inkwell_ble_services_resolved(&central, ADDRESS, &resolved);
    const int not_yet = inkwell_ble_services_resolved(&central, ADDRESS, &resolved);
    const bool first = resolved;
    (void)inkwell_ble_services_resolved(&central, ADDRESS, &resolved);
    bool up_first = false;
    bool up_second = true;
    (void)inkwell_ble_device_connected(&central, ADDRESS, &up_first);
    (void)inkwell_ble_device_connected(&central, ADDRESS, &up_second);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(busy != -ETIMEDOUT, "a busy stack's timeout was not reported");
    INKWELL_TEST_FAIL_IF(not_yet != 0 || first, "services resolved before discovery finished");
    INKWELL_TEST_FAIL_IF(!resolved, "services never resolved");
    INKWELL_TEST_FAIL_IF(!up_first || up_second, "the link did not drop when scripted to");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_close_forgets_the_central, unit) {
    inkwell_ble_mock_enable(NULL);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    struct received received = {0};
    inkwell_ble_set_notification_handler(&central, on_notification, &received);
    (void)inkwell_ble_subscribe(&central, "h");
    inkwell_ble_close(&central);
    const uint8_t value[] = {1};
    inkwell_ble_mock_emit_notification("h", value, sizeof value);
    const int processed = inkwell_ble_process(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(received.calls != 0U, "a closed central still received notifications");
    INKWELL_TEST_FAIL_IF(processed != -ENOTCONN, "a closed central still processed");
    record_success(test_name);
}

INKWELL_TEST_CASE(ble_mtu_is_optional, unit) {
    const struct inkwell_ble_mock_config none = {0};
    inkwell_ble_mock_enable(&none);
    struct inkwell_ble_central central;
    (void)inkwell_ble_open(&central);
    uint16_t mtu = 1U;
    const int missing = inkwell_ble_characteristic_mtu(&central, "h", &mtu);
    inkwell_ble_close(&central);

    const struct inkwell_ble_mock_config some = {.mtu = 247U};
    inkwell_ble_mock_enable(&some);
    (void)inkwell_ble_open(&central);
    uint16_t negotiated = 0U;
    const int present = inkwell_ble_characteristic_mtu(&central, "h", &negotiated);
    inkwell_ble_close(&central);
    inkwell_ble_mock_disable();

    INKWELL_TEST_FAIL_IF(missing != -ENOTSUP || mtu != 0U, "an absent MTU was not -ENOTSUP");
    INKWELL_TEST_FAIL_IF(present != 0 || negotiated != 247U, "the MTU was not reported");
    record_success(test_name);
}

#if defined(INKWELL_BLE_BACKEND_none)
/* The claim the stub makes: with no stack, opening refuses, and nothing else is reachable. */
INKWELL_TEST_CASE(ble_without_a_stack_refuses, unit) {
    struct inkwell_ble_central central;
    const int opened = inkwell_ble_open(&central);
    char name[32];
    const int adapter = inkwell_ble_find_adapter(&central, name, sizeof name);
    INKWELL_TEST_FAIL_IF(opened != -ENOSYS, "a build with no stack opened a central");
    INKWELL_TEST_FAIL_IF(adapter != -ENOTCONN, "an unopened central found an adapter");
    record_success(test_name);
}
#endif
