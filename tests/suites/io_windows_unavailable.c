#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"
#include "inkwell/io/serial.h"
#include "inkwell/io/usb_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <string.h>

INKWELL_TEST_CASE(io_windows_serial_refuses_without_a_backend, unit) {
    struct inkwell_serial_port_info port = {0};
    struct inkwell_serial_port_info found[1];
    INKWELL_TEST_FAIL_IF(inkwell_serial_scan(found, 1U) != 0U, "scan should find no ports");
    INKWELL_TEST_FAIL_IF(inkwell_serial_bind(NULL) != -EINVAL, "NULL bind should be invalid");
    INKWELL_TEST_FAIL_IF(inkwell_serial_bind(&port) != -ENOTSUP, "bind should be unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_serial_set_line_state(&port, true, false) != -ENOTSUP,
                         "line state should be unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_serial_open("COM1", 115200U) != -ENOTSUP,
                         "open should be unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_serial_open("", 115200U) != -EINVAL,
                         "empty path should be invalid");
    INKWELL_TEST_FAIL_IF(inkwell_serial_set_dtr(0, true) != -ENOTSUP, "DTR should be unavailable");
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

INKWELL_TEST_CASE(io_windows_storage_refuses_without_a_backend, unit) {
    struct inkwell_serial_port_info port = {0};
    struct inkwell_usb_storage_target target;
    memset(&target, 0xA5, sizeof target);
    INKWELL_TEST_FAIL_IF(inkwell_usb_storage_find(&port, &target) != -ENOTSUP,
                         "storage discovery should be unavailable");
    INKWELL_TEST_FAIL_IF(target.device[0] != '\0' || target.mount_count != 0U,
                         "failed discovery should clear the output");
    INKWELL_TEST_FAIL_IF(inkwell_usb_storage_unmount(&target) != -ENOTSUP,
                         "unmount should be unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_usb_storage_claim("disk") != -ENOTSUP,
                         "exclusive claim should be unavailable");

    struct inkwell_usb_storage_write write;
    inkwell_usb_storage_write_init(&write);
    const uint8_t image[] = {1U};
    INKWELL_TEST_FAIL_IF(
        inkwell_usb_storage_write_start(&write, NULL, image, sizeof image, "disk", 0U) != -ENOTSUP,
        "raw write should be unavailable");
    INKWELL_TEST_FAIL_IF(write.state != INKWELL_USB_STORAGE_WRITE_IDLE ||
                             inkwell_usb_storage_write_progress(&write) != 0U,
                         "failed writer should remain idle");
    inkwell_usb_storage_write_cancel(&write);
    record_success(test_name);
}
