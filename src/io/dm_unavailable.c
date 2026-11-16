#include "inkwell/io/dm.h"

#include <errno.h>

/* The device mapper is Linux's; elsewhere making and removing devices refuses. */

int inkwell_dm_create(const char *name, const struct inkwell_dm_target *targets, size_t count,
                      bool read_only, struct inkwell_dm_device *out) {
    (void)name;
    (void)targets;
    (void)count;
    (void)read_only;
    (void)out;
    return -ENOSYS;
}

int inkwell_dm_remove(const char *name) {
    (void)name;
    return -ENOSYS;
}
