#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"
#include "inkwell/io/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <string.h>

/* Every port the scan reports is one the rest of the API can open by name: on USB, with its COM
   name, and needing no bind. A CI runner has none, which is a scan of zero and still a pass. */
INKWELL_TEST_CASE(io_windows_serial_scan_reports_openable_usb_ports, unit) {
    struct inkwell_serial_port_info found[8];
    memset(found, 0, sizeof found);
    const size_t count = inkwell_serial_scan(found, 8U);
    for (size_t i = 0; i < count; ++i) {
        INKWELL_TEST_FAIL_IF(strncmp(found[i].path, "COM", 3) != 0, "a port has its COM name");
        INKWELL_TEST_FAIL_IF(!found[i].bound || found[i].needs_line_state,
                             "a COM port needs no bind and no out-of-band line state");
        INKWELL_TEST_FAIL_IF(found[i].vendor_id == 0U || found[i].name[0] == '\0',
                             "a port is a USB device with a name");
        INKWELL_TEST_FAIL_IF(inkwell_serial_bind(&found[i]) != 0, "a listed port is bound");
    }
    const size_t nowhere = inkwell_serial_scan(NULL, 1U) + inkwell_serial_scan(found, 0U);
    INKWELL_TEST_FAIL_IF(nowhere != 0U, "a scan with nowhere to write finds nothing");
    record_success(test_name);
}

INKWELL_TEST_CASE(io_windows_serial_refuses_what_it_cannot_open, unit) {
    struct inkwell_serial_port_info unbound = {0};
    INKWELL_TEST_FAIL_IF(inkwell_serial_bind(NULL) != -EINVAL, "NULL bind should be invalid");
    INKWELL_TEST_FAIL_IF(inkwell_serial_bind(&unbound) != -ENOTSUP,
                         "there is no driver to bind on Windows");
    INKWELL_TEST_FAIL_IF(inkwell_serial_set_line_state(&unbound, true, false) != -ENOTSUP,
                         "the line state never goes around usbser");
    INKWELL_TEST_FAIL_IF(inkwell_serial_open("", 115200U) != -EINVAL,
                         "empty path should be invalid");
    INKWELL_TEST_FAIL_IF(inkwell_serial_open("COM250", 115200U) != -ENOENT,
                         "a port that is not there is not found");
    INKWELL_TEST_FAIL_IF(inkwell_serial_set_dtr(-1, true) != -EINVAL, "no descriptor, no DTR");
    INKWELL_TEST_FAIL_IF(inkwell_serial_set_dtr(0, true) != -ENOTTY,
                         "a descriptor that is not a port has no DTR");
    record_success(test_name);
}

INKWELL_TEST_CASE(io_windows_serial_keeps_the_mock_seam, unit) {
    struct inkwell_serial_port_info scripted = {0};
    struct inkwell_serial_port_info found[1];
    memset(found, 0, sizeof found);
    const struct inkwell_serial_mock_config config = {
        .ports = &scripted,
        .port_count = 1U,
        .bind_pending_polls = 1U,
        .bound_path = "COM9",
        .open_fd = -1,
    };
    inkwell_serial_mock_enable(&config);
    const bool scanned = inkwell_serial_scan(found, 1U) == 1U;
    const int pending = inkwell_serial_bind(&found[0]);
    const int bound = inkwell_serial_bind(&found[0]);
    const int line = inkwell_serial_set_line_state(&found[0], true, false);
    const int opened = inkwell_serial_open(found[0].path, 115200U);
    const bool recorded =
        inkwell_serial_mock_bind_calls() == 2U && inkwell_serial_mock_line_state_calls() == 1U;
    inkwell_serial_mock_disable();
    INKWELL_TEST_FAIL_IF(!scanned || pending != -EAGAIN || bound != 0 ||
                             strcmp(found[0].path, "COM9") != 0 || line != 0 || opened != -ENOENT ||
                             !recorded,
                         "mocked discovery and bind should remain usable");
    record_success(test_name);
}

INKWELL_TEST_CASE(io_windows_serial_mock_duplicates_its_descriptor, unit) {
    int fds[2];
    INKWELL_TEST_FAIL_IF(_pipe(fds, 64U, _O_BINARY) != 0, "fixture pipe should open");
    const struct inkwell_serial_mock_config config = {.open_fd = fds[0]};
    inkwell_serial_mock_enable(&config);
    const int opened = inkwell_serial_open("COM9", 115200U);
    inkwell_serial_mock_disable();
    (void)inkwell_fd_close(fds[0]);
    const uint8_t byte = 7U;
    const int sent = inkwell_fd_write(fds[1], &byte, 1U);
    uint8_t received = 0U;
    const int got = opened >= 0 ? inkwell_fd_read(opened, &received, 1U) : -1;
    inkwell_serial_close(opened);
    (void)inkwell_fd_close(fds[1]);
    INKWELL_TEST_FAIL_IF(opened < 0 || sent != 1 || got != 1 || received != byte,
                         "mocked open should own an independent descriptor");
    record_success(test_name);
}
