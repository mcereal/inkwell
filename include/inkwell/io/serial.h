#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB serial ports: which ones there are, what is on the other end of each, and opening one.
 *
 * The scan reads sysfs on Linux and the I/O Registry on macOS, and elsewhere finds nothing. It
 * reports what the USB tree says - the interface, the device it hangs off, whether that device
 * is a UART bridge or its own MCU's USB, whether a mass-storage interface sits beside it - and
 * never what the device *is for*. Which of these ports is worth talking to is the caller's
 * question. Until a native Windows backend exists, the scan reports no ports and device
 * operations return -ENOTSUP; the mock seam remains available for tests.
 *
 * Two of the calls exist for kernels without CDC-ACM - the TrimUI Brick's TinaLinux 4.9 has
 * CONFIG_USB_ACM off and no module, so a native-USB device enumerates and then gets no driver
 * and no /dev/ttyACM*. The workaround, verified on that device:
 *
 * - `inkwell_serial_bind()` writes "VID PID" to /sys/bus/usb-serial/drivers/generic/new_id. The
 *   generic driver rejects the control interface ("no bulk out") and attaches the data one as
 *   /dev/ttyUSB*, usually at once and sometimes a little later - so the bind answers -EAGAIN
 *   until the tty is there rather than sleeping on the loop.
 * - `inkwell_serial_set_line_state()` then sends one CDC SET_CONTROL_LINE_STATE through usbfs
 *   (/dev/bus/usb/BBB/DDD) against the unbound control interface, because TinyUSB discards
 *   output until the host sets DTR and the generic driver cannot.
 *
 * Neither survives a reboot. A UART bridge (CP2102, CH341, FTDI) needs neither: its driver is
 * present, its tty already exists, and DTR is a plain TIOCMBIS.
 *
 * Everything here is mockable, so a test never touches sysfs, usbfs or a real tty.
 */

/* What the USB device behind a port is. */
enum inkwell_serial_kind {
    /* The MCU's own USB: a CDC-Data interface. */
    INKWELL_SERIAL_NATIVE,
    /* A UART bridge chip on a vendor-class interface. USB can say nothing about what is wired
       to its far side. */
    INKWELL_SERIAL_BRIDGE,
};

struct inkwell_serial_port_info {
    /* Stable across a replug into the same socket: the sysfs interface name ("1-1:1.1") on
       Linux, the callout path on macOS. */
    char id[64];
    /* "/dev/ttyUSB0"; empty until the interface has a driver bound. */
    char path[64];
    /* The USB product string, falling back to "USB serial VVVV:PPPP". */
    char name[64];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t busnum;
    uint8_t devnum;
    /* bInterfaceNumber of the CDC control interface on the same device, or -1 when there is
       none to set the line state on. */
    int control_interface;
    /* A tty node exists for this interface right now. */
    bool bound;
    /* Bound by a driver that cannot drive DTR: the line state has to go through usbfs after
       the port is open. */
    bool needs_line_state;
    enum inkwell_serial_kind kind;
    /* The bind's own bookkeeping: new_id has been written for this port, and a later call only
       looks for the tty. */
    bool bind_requested;
    /* A mass-storage Bulk-Only interface (08/06/50) sits on the same device - what a UF2
       bootloader presents beside its CDC pair. Only ever set on a native port. */
    bool mass_storage;
};

/* Fills `out` with the USB serial ports: interfaces that have published a tty or are bound to a
   usb-serial driver, plus unbound CDC-Data interfaces that `inkwell_serial_bind()` could bind. A
   CDC function is one port, reported at its data interface even when the tty hangs off its
   control interface, as cdc_acm's does. Returns how many were written, at most `capacity`. */
size_t inkwell_serial_scan(struct inkwell_serial_port_info *out, size_t capacity);

/* Binds an unbound CDC-Data interface to the generic usbserial driver, filling in `path` and
   `bound` once its tty appears. Returns 0 when it has, -EAGAIN while it has not - call again
   with the same `port` until it answers something else, and give up when the caller's patience
   does (a second has always been plenty) - or a negative errno. A port that is already bound
   answers 0 at once. Linux only; -ENOTSUP elsewhere. */
int inkwell_serial_bind(struct inkwell_serial_port_info *port);

/* One CDC SET_CONTROL_LINE_STATE to the port's control interface through usbfs. Returns 0,
   -ENOTSUP when there is no control interface or no usbfs, or a negative errno. */
int inkwell_serial_set_line_state(const struct inkwell_serial_port_info *port, bool dtr, bool rts);

/*
 * Opens the tty raw and non-blocking at `baud` (meaningless over USB CDC, honoured by bridges).
 * Returns the fd, -EINVAL for a rate this platform's termios has no constant for, or a negative
 * errno.
 *
 * VMIN is 1, so an empty buffer reads as EAGAIN and a zero-length read means the device went
 * away - which is what inkwell's stream (`net/stream.h`) takes it to mean.
 */
int inkwell_serial_open(const char *path, unsigned baud);
void inkwell_serial_close(int fd);

/* TIOCMBIS/TIOCMBIC on the tty. Returns 0 or a negative errno; -ENOTTY when the fd is not a
   tty, as for the generic-driver ports that need `inkwell_serial_set_line_state()` instead. */
int inkwell_serial_set_dtr(int fd, bool on);

/* Reads the USB tree from `root` instead of /sys/bus/usb/devices - on macOS too, where it
   replaces the I/O Registry - so a test can lay one out on disk. NULL restores the default. A
   test seam: nothing outside a test should call it. */
void inkwell_serial_set_sysfs_root(const char *root);

struct inkwell_serial_mock_config {
    const struct inkwell_serial_port_info *ports;
    size_t port_count;
    int scan_result; /* < 0 makes the scan report nothing */
    int bind_result; /* returned by inkwell_serial_bind */
    /* How many binds of an unbound port answer -EAGAIN before one succeeds. */
    unsigned bind_pending_polls;
    int line_state_result; /* returned by inkwell_serial_set_line_state */
    /* The path a successful bind reports for a port the scan found unbound. */
    const char *bound_path;
    /* When >= 0, inkwell_serial_open dup()s this instead of opening a tty: a test hands it one
       end of a socketpair and scripts the device from the other. */
    int open_fd;
    int open_result; /* < 0 makes every open fail with this */
};

/* NULL is a mock with no ports and no fd to open. */
void inkwell_serial_mock_enable(const struct inkwell_serial_mock_config *config);
void inkwell_serial_mock_disable(void);
/* How many binds and line-state requests were asked for since the mock was enabled. */
size_t inkwell_serial_mock_bind_calls(void);
size_t inkwell_serial_mock_line_state_calls(void);

#ifdef __cplusplus
}
#endif
