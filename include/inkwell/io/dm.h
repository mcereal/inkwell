#pragma once

/*
 * The device mapper: a block device the kernel makes out of others by following a table - a
 * slice of a disk (linear), a checked read-only view of one (verity), an encrypted one (crypt).
 * It is what `dmsetup create` and `veritysetup open` do, as calls, over the ioctls on
 * /dev/mapper/control.
 *
 * A table is one or more targets, each a run of sectors given to one target type with its
 * parameters as the kernel parses them: a line of text. The text is the kernel's interface and
 * this header does not re-model it, with one exception: dm-verity's line, which is long, ordered
 * and easy to get wrong, has a formatter (inkwell_dm_verity_params()) that is plain text and
 * tested everywhere.
 *
 * A device made here is live when the call returns: created, loaded and resumed, its node made
 * by devtmpfs as /dev/dm-<minor>. A failure part-way removes what was made, so a name is never
 * left half-built. Devices are named by the caller; a name taken is -EEXIST.
 *
 * Needs CAP_SYS_ADMIN and a kernel with the target types used (dm-mod, dm-verity). Off Linux
 * every call but the formatter refuses with -ENOSYS.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INKWELL_DM_NAME_MAX 128U /* DM_NAME_LEN */
#define INKWELL_DM_TYPE_MAX 16U  /* DM_MAX_TYPE_NAME */

struct inkwell_dm_target {
    uint64_t start;   /* in 512-byte sectors, from the device's start */
    uint64_t sectors; /* how many */
    const char *type; /* "linear", "verity", ... */
    const char *params;
};

struct inkwell_dm_device {
    uint32_t major;
    uint32_t minor;
    char path[32]; /* "/dev/dm-<minor>" */
};

/*
 * Makes device `name` from `count` targets, read-only when `read_only`, and resumes it. 0 with
 * `out` filled (when not NULL), or a negative errno: -EEXIST for a name taken, -EINVAL for a
 * table the kernel refused (a target type it lacks, parameters it cannot parse - its reason is
 * in the kernel log).
 */
int inkwell_dm_create(const char *name, const struct inkwell_dm_target *targets, size_t count,
                      bool read_only, struct inkwell_dm_device *out);

/* Removes device `name`. 0, -ENXIO when there is none, -EBUSY while it is mounted or open. */
int inkwell_dm_remove(const char *name);

/*
 * dm-verity: a device whose every block read is checked against a hash tree, whose root is the
 * one value to trust. The data and the tree may share a device, the tree after the data, as
 * `veritysetup format --no-superblock --hash-offset` lays them out.
 */
struct inkwell_dm_verity {
    /* "major:minor" or a path, as the kernel takes either */
    const char *data_device;
    const char *hash_device;
    uint32_t data_block_size; /* bytes; 4096 */
    uint32_t hash_block_size;
    uint64_t data_blocks;      /* how many data blocks the tree covers */
    uint64_t hash_start_block; /* where the tree starts on hash_device, in hash blocks */
    const char *algorithm;     /* "sha256" */
    const uint8_t *root_digest;
    size_t root_digest_len;
    const uint8_t *salt; /* may be empty */
    size_t salt_len;
    /* Optional arguments as the kernel counts them - "1 restart_on_corruption" - or NULL. */
    const char *options;
};

/* The verity target's parameter line, format version 1, into `out`. Its length, or -EINVAL for
   a verity missing a field, or -ENOSPC when `size` is too small. Its sectors are data_blocks *
   data_block_size / 512. */
int inkwell_dm_verity_params(const struct inkwell_dm_verity *verity, char *out, size_t size);

#ifdef __cplusplus
}
#endif
