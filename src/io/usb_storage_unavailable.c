#include "inkwell/io/usb_storage.h"

#include <errno.h>
#include <string.h>

/* A Windows storage target needs volume discovery, safe dismount and exclusive raw access.
 * None is approximated with a normal file open: that would allow a filesystem to write back
 * while raw image bytes are being sent to the same device. */
int inkwell_usb_storage_find(const struct inkwell_serial_port_info *port,
                             struct inkwell_usb_storage_target *out) {
    if (port == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);
    return -ENOTSUP;
}

int inkwell_usb_storage_unmount(struct inkwell_usb_storage_target *target) {
    return target == NULL ? -EINVAL : -ENOTSUP;
}

int inkwell_usb_storage_claim(const char *device_path) {
    return device_path == NULL || device_path[0] == '\0' ? -EINVAL : -ENOTSUP;
}

void inkwell_usb_storage_write_init(struct inkwell_usb_storage_write *write) {
    if (write == NULL) {
        return;
    }
    memset(write, 0, sizeof *write);
    write->child = -1;
    write->progress_fd = -1;
    write->device_fd = -1;
}

int inkwell_usb_storage_write_start(struct inkwell_usb_storage_write *write,
                                    struct inkwell_loop *loop, const uint8_t *image, size_t len,
                                    const char *device_path, uint64_t now_ms) {
    (void)loop;
    (void)image;
    (void)len;
    (void)device_path;
    (void)now_ms;
    return write == NULL ? -EINVAL : -ENOTSUP;
}

void inkwell_usb_storage_write_tick(struct inkwell_usb_storage_write *write, uint64_t now_ms) {
    (void)write;
    (void)now_ms;
}

void inkwell_usb_storage_write_cancel(struct inkwell_usb_storage_write *write) {
    if (write != NULL) {
        write->state = INKWELL_USB_STORAGE_WRITE_IDLE;
    }
}

unsigned inkwell_usb_storage_write_progress(const struct inkwell_usb_storage_write *write) {
    (void)write;
    return 0U;
}
