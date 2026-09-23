#pragma once

#include "inkwell/io/serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* Refuse a device with more mountpoints than can be named: partial unmounting leaves a
   filesystem able to write back while the raw device is being written. */
#define INKWELL_USB_STORAGE_MOUNTS_MAX 4U
#define INKWELL_USB_STORAGE_PATH_MAX 128U

struct inkwell_usb_storage_target {
    char device[96];
    uint64_t size_bytes;
    char mounts[INKWELL_USB_STORAGE_MOUNTS_MAX][INKWELL_USB_STORAGE_PATH_MAX];
    size_t mount_count;
    bool too_many_mounts;
};

/* Find the block device published by the USB device containing a serial interface. An interface
   id such as "2-1:1.2" is matched at its device directory, "2-1", so a sibling storage interface
   is found too. Returns -ENOENT while the block device has not appeared. Linux sysfs only;
   until a native backend exists, Windows returns -ENOTSUP for discovery and writes. */
int inkwell_usb_storage_find(const struct inkwell_serial_port_info *port,
                             struct inkwell_usb_storage_target *out);

/* Unmount every reported mountpoint in reverse mount order, without lazy detach. Returns -E2BIG
   without touching any mountpoint if the target has more than the struct can hold. */
int inkwell_usb_storage_unmount(struct inkwell_usb_storage_target *target);

/* Open an existing device for writing with O_EXCL. For block devices this prevents a filesystem
   from mounting it while the returned fd is held. A mount that wins the race is discovered,
   unmounted and retried; an unrelated holder is refused with -EBUSY. Never creates a path. */
int inkwell_usb_storage_claim(const char *device_path);

enum inkwell_usb_storage_write_state {
    INKWELL_USB_STORAGE_WRITE_IDLE = 0,
    INKWELL_USB_STORAGE_WRITE_RUNNING,
    INKWELL_USB_STORAGE_WRITE_DONE,
    INKWELL_USB_STORAGE_WRITE_FAILED,
};

struct inkwell_usb_storage_write {
    struct inkwell_loop *loop;
    enum inkwell_usb_storage_write_state state;
    int error;
    pid_t child;
    int progress_fd;
    /* The parent retains the exclusive claim until the child is reaped. */
    int device_fd;
    char pending[32];
    size_t pending_len;
    uint64_t written;
    uint64_t total;
    uint64_t idle_deadline_ms;
};

/* Initialize a writer before its first start, tick, cancel, or progress call. Call again only
   after a previous write has finished or been cancelled; initializing a running writer would
   discard its child and exclusive device claim. An initialized idle writer owns no descriptors. */
void inkwell_usb_storage_write_init(struct inkwell_usb_storage_write *write);

/* Fork a child to write bytes to an existing block device. Each chunk is synced before its
   progress is reported. The caller owns image until the writer finishes or is cancelled; a
   loop is optional, and tick must be called even when one is supplied to reap the child. */
int inkwell_usb_storage_write_start(struct inkwell_usb_storage_write *write,
                                    struct inkwell_loop *loop, const uint8_t *image, size_t len,
                                    const char *device_path, uint64_t now_ms);
void inkwell_usb_storage_write_tick(struct inkwell_usb_storage_write *write, uint64_t now_ms);
void inkwell_usb_storage_write_cancel(struct inkwell_usb_storage_write *write);
unsigned inkwell_usb_storage_write_progress(const struct inkwell_usb_storage_write *write);

#ifdef __cplusplus
}
#endif
