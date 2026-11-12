#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/*
 * The kernel's device events: a device, a driver binding, a controller appearing or going away,
 * as the kernel announces it on a NETLINK_KOBJECT_UEVENT socket. It is what a program waits on
 * instead of polling /sys for something to show up.
 *
 * Two halves. The parser reads one message - "add@/devices/...\0ACTION=add\0DEVPATH=...\0" - and
 * is plain bytes, so it builds and is tested on every system. The monitor is the socket on the
 * loop, and is Linux's; elsewhere every monitor call refuses with -ENOSYS.
 *
 * Only the kernel's own messages are taken: the socket joins the kernel's multicast group, and
 * a message whose sender is not the kernel (netlink port 0) is dropped. udevd re-broadcasts
 * events on a second group in its own format ("libudev" header, binary); that is udev's
 * interface, not the kernel's, and the parser refuses it.
 *
 * **Events cannot be replayed.** A device that was already there before the monitor opened was
 * announced to nobody. The order that loses nothing is: open the monitor, then look at /sys
 * for what is already present - or ask the kernel to announce it again, which is what
 * inkwell_uevent_trigger() does and what `udevadm trigger` does underneath. Either way an event
 * may be seen twice, so act on the state a device is in, not on the count of events.
 *
 * **Events can be lost.** When the socket's buffer overflows, the kernel drops events and the
 * next receive says so. The monitor reports that as a callback with `event` NULL: what was
 * missed is unknowable, and the only answer is to look at /sys again.
 *
 * Which network namespace a process is in matters: the kernel sends a device's events into the
 * namespace that owns it, which for physical hardware is the initial one. A process in a
 * container with its own network namespace opens the socket fine and hears nothing.
 */

/* One event. Every pointer is into the buffer it was parsed from, and lives as long as it. */
struct inkwell_uevent {
    const char *action;    /* "add", "remove", "change", "move", "bind", "unbind", ... */
    const char *devpath;   /* "/devices/platform/...": sysfs, without the "/sys" */
    const char *subsystem; /* "usb", "udc", "input", ...; NULL when the kernel sent none */
    const char *devname;   /* "input/event3", relative to /dev; NULL without a device node */
    const char *devtype;   /* NULL when the kernel sent none */
    uint64_t seqnum;       /* the kernel's running count; 0 when absent */
    /* The KEY=VALUE strings, each NUL-terminated, as the kernel sent them: for
       inkwell_uevent_get(), or to walk. */
    const char *env;
    size_t env_len;
};

/*
 * Parses one kernel message of `len` bytes, which must end in a NUL as the kernel's do. 0, or
 * -EINVAL for something that is not one: no "action@devpath" header (udevd's format among
 * them), no ACTION or DEVPATH, or a header that disagrees with them.
 */
int inkwell_uevent_parse(const char *buf, size_t len, struct inkwell_uevent *out);

/* The value of `key` ("MAJOR", "DRIVER", "MODALIAS", ...), or NULL. */
const char *inkwell_uevent_get(const struct inkwell_uevent *event, const char *key);

/* `event` is NULL when events were lost; see above. */
typedef void (*inkwell_uevent_callback)(const struct inkwell_uevent *event, void *userdata);

struct inkwell_uevent_monitor {
    struct inkwell_loop *loop; /* NULL while closed */
    int fd;
    inkwell_uevent_callback callback;
    void *userdata;
};

/* Opens the socket and watches it on `loop`. 0, or a negative errno with the monitor closed;
   -ENOSYS off Linux. A zeroed monitor is a closed one. */
int inkwell_uevent_monitor_open(struct inkwell_uevent_monitor *monitor, struct inkwell_loop *loop,
                                inkwell_uevent_callback callback, void *userdata);

/* Safe on a closed monitor, and from inside its own callback. */
void inkwell_uevent_monitor_close(struct inkwell_uevent_monitor *monitor);

/*
 * Asks the kernel to announce a device again: writes `action` ("add", "change") to
 * `<syspath>/uevent`, where `syspath` is the device's directory under /sys. The event arrives
 * on every monitor like any other. Needs write access to sysfs, which is root's. 0 or a negative
 * errno; -ENOSYS off Linux.
 */
int inkwell_uevent_trigger(const char *syspath, const char *action);

#ifdef __cplusplus
}
#endif
