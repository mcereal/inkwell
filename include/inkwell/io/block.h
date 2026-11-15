#pragma once

/*
 * The system's block devices, found by what a partition table calls them.
 *
 * A disk's partitions are numbered by where they sit (vda2, sda14, mmcblk0p3), and the numbers
 * differ from one device to the next; what stays put is the name the partition table gives each
 * one - GPT's partition name, "system_a" or "userdata" - which the kernel reports as PARTNAME in
 * the partition's uevent file under /sys/class/block. That is the lookup here: a name in, the
 * device node and its size out, read from sysfs as `blkid` or Android's by-name links would.
 *
 * A disk's partitions appear once its driver has probed it, which may be after the program that
 * wants one has started. -ENOENT says "not here yet" as much as "not here"; a caller that should
 * wait opens a uevent monitor (io/uevent.h) first and asks again on each block event, as that
 * header says to.
 *
 * The sysfs root is a parameter - "/sys" on a device - so the lookup runs against a copy of the
 * tree a test made, on any POSIX system. On Windows every call refuses with -ENOSYS.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPT's partition name is 36 UTF-16 code units; the kernel hands it on as UTF-8. */
#define INKWELL_BLOCK_NAME_MAX 109U
#define INKWELL_BLOCK_PATH_MAX 64U

struct inkwell_block_partition {
    char path[INKWELL_BLOCK_PATH_MAX]; /* "/dev/vda2": the node devtmpfs makes for DEVNAME */
    uint32_t major;
    uint32_t minor;
    uint64_t size; /* bytes */
};

/*
 * The partition named `name` under the sysfs mounted at `sys`. 0; -ENOENT when no partition has
 * that name; -EINVAL for an empty name or one longer than a partition's; another negative errno
 * when sysfs cannot be read. Two partitions with one name (a second disk with the same table) is
 * -EEXIST: which of them is meant is not something a name can say.
 */
int inkwell_block_find(const char *sys, const char *name, struct inkwell_block_partition *out);

/*
 * The value of `key` in the text of a uevent file - "MAJOR=253\nMINOR=2\nPARTNAME=misc\n" -
 * copied into `out`. 0, -ENOENT when the key is absent, -ENOSPC when the value does not fit.
 * Plain text, so it is the part tested everywhere.
 */
int inkwell_block_uevent_value(const char *text, const char *key, char *out, size_t size);

#ifdef __cplusplus
}
#endif
