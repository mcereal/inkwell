#include "inkwell/io/uevent.h"

#include <errno.h>

/* Kernel device events are Linux's netlink. macOS announces devices through IOKit
 * notifications and Windows through device-interface notifications; either would be its own
 * backend with its own vocabulary, so until one is wanted every call refuses. The parser in
 * uevent.c is plain bytes and works everywhere. */
int inkwell_uevent_monitor_open(struct inkwell_uevent_monitor *monitor, struct inkwell_loop *loop,
                                inkwell_uevent_callback callback, void *userdata) {
    (void)loop;
    (void)callback;
    (void)userdata;
    return monitor == NULL ? -EINVAL : -ENOSYS;
}

void inkwell_uevent_monitor_close(struct inkwell_uevent_monitor *monitor) {
    (void)monitor;
}

int inkwell_uevent_trigger(const char *syspath, const char *action) {
    (void)syspath;
    (void)action;
    return -ENOSYS;
}
