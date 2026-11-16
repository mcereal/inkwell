#include "inkwell/io/block.h"

#include <errno.h>

/* No sysfs here: the lookup refuses. The uevent text parser is block.c's. */

int inkwell_block_find(const char *sys, const char *name, struct inkwell_block_partition *out) {
    (void)sys;
    (void)name;
    (void)out;
    return -ENOSYS;
}
