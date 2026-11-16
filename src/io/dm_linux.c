#include "inkwell/io/dm.h"

#include "inkwell/base/ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The device mapper's control ioctls. Every request is one buffer: a struct dm_ioctl, and after
 * it (at data_start) whatever the request carries - for a table load, each target as a struct
 * dm_target_spec followed by its parameter string, the next spec at `next` bytes from this one,
 * 8-byte aligned. The kernel answers in the same buffer.
 */

#define CONTROL "/dev/mapper/control"
#define BUFFER_SIZE 16384U

static _Alignas(8) unsigned char g_buf[BUFFER_SIZE];

static struct dm_ioctl *request(const char *name, uint32_t flags) {
    memset(g_buf, 0, sizeof g_buf);
    struct dm_ioctl *io = (struct dm_ioctl *)(void *)g_buf;
    io->version[0] = DM_VERSION_MAJOR;
    io->version[1] = 0;
    io->version[2] = 0;
    io->data_size = BUFFER_SIZE;
    io->data_start = sizeof *io;
    io->flags = flags;
    snprintf(io->name, sizeof io->name, "%s", name);
    return io;
}

static int call(int fd, unsigned long cmd) {
    for (;;) {
        if (ioctl(fd, inkwell_ioctl_request_of(cmd), g_buf) == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
}

static int remove_with(int fd, const char *name) {
    (void)request(name, 0U);
    return call(fd, DM_DEV_REMOVE);
}

/* The targets after the header, as DM_TABLE_LOAD takes them. 0 or -EINVAL / -ENOSPC. */
static int lay_out(struct dm_ioctl *io, const struct inkwell_dm_target *targets, size_t count) {
    size_t at = io->data_start;
    for (size_t i = 0U; i < count; ++i) {
        const struct inkwell_dm_target *t = &targets[i];
        if (t->type == NULL || t->params == NULL || strlen(t->type) >= INKWELL_DM_TYPE_MAX ||
            t->sectors == 0U) {
            return -EINVAL;
        }
        const size_t params = strlen(t->params) + 1U;
        const size_t len = (sizeof(struct dm_target_spec) + params + 7U) & ~(size_t)7U;
        if (at + len > BUFFER_SIZE) {
            return -ENOSPC;
        }
        struct dm_target_spec *spec = (struct dm_target_spec *)(void *)(g_buf + at);
        spec->sector_start = t->start;
        spec->length = t->sectors;
        snprintf(spec->target_type, sizeof spec->target_type, "%s", t->type);
        /* The parameter string follows its spec. */
        memcpy(spec + 1, t->params, params);
        spec->next = (uint32_t)len;
        at += len;
    }
    io->target_count = (uint32_t)count;
    io->data_size = (uint32_t)at;
    return 0;
}

int inkwell_dm_create(const char *name, const struct inkwell_dm_target *targets, size_t count,
                      bool read_only, struct inkwell_dm_device *out) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= INKWELL_DM_NAME_MAX || targets == NULL ||
        count == 0U) {
        return -EINVAL;
    }
    const int fd = open(CONTROL, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    (void)request(name, 0U);
    int err = call(fd, DM_DEV_CREATE);
    if (err != 0) {
        (void)close(fd);
        return err == -EBUSY ? -EEXIST : err;
    }
    struct dm_ioctl *io = request(name, read_only ? DM_READONLY_FLAG : 0U);
    err = lay_out(io, targets, count);
    if (err == 0) {
        err = call(fd, DM_TABLE_LOAD);
    }
    if (err == 0) {
        /* A suspend without DM_SUSPEND_FLAG resumes: the loaded table goes live. */
        io = request(name, 0U);
        err = call(fd, DM_DEV_SUSPEND);
    }
    if (err == 0 && out != NULL) {
        /* The kernel's huge_encode_dev(): minor's low byte, then major, then minor's high bits. */
        const uint64_t dev = io->dev;
        out->major = (uint32_t)((dev >> 8) & 0xfffU);
        out->minor = (uint32_t)((dev & 0xffU) | ((dev >> 12) & 0xfff00U));
        snprintf(out->path, sizeof out->path, "/dev/dm-%u", (unsigned)out->minor);
    }
    if (err != 0) {
        (void)remove_with(fd, name);
    }
    (void)close(fd);
    return err;
}

int inkwell_dm_remove(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= INKWELL_DM_NAME_MAX) {
        return -EINVAL;
    }
    const int fd = open(CONTROL, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    const int err = remove_with(fd, name);
    (void)close(fd);
    return err;
}
