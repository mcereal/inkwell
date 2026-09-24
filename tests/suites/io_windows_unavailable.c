#include "framework/inkwell_test.h"

#include "inkwell/io/serial.h"
#include "inkwell/io/usb_storage.h"

#include <errno.h>
#include <string.h>

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
