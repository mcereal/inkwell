#include "inkwell/io/block.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Partitions found by name under sysfs: any POSIX system, given a tree.

   A partition's uevent file is a few short lines; sysfs hands out at most a page. */
#define UEVENT_MAX 4096U

/* The whole of a small file under sysfs, NUL-terminated. Its length, or a negative errno. */
static ssize_t read_small(const char *path, char *buf, size_t size) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    size_t used = 0U;
    while (used + 1U < size) {
        const ssize_t n = read(fd, buf + used, size - 1U - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            (void)close(fd);
            return -err;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    (void)close(fd);
    buf[used] = '\0';
    return (ssize_t)used;
}

static bool parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    const unsigned long v = strtoul(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\0' && *end != '\n') || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* One entry of class/block, filled in when its PARTNAME is `name`. 1 for a match, 0 for none. */
static int match(const char *sys, const char *entry, const char *name,
                 struct inkwell_block_partition *out) {
    char path[512];
    char text[UEVENT_MAX];
    snprintf(path, sizeof path, "%s/class/block/%s/uevent", sys, entry);
    if (read_small(path, text, sizeof text) < 0) {
        return 0;
    }
    char value[INKWELL_BLOCK_NAME_MAX];
    if (inkwell_block_uevent_value(text, "PARTNAME", value, sizeof value) != 0 ||
        strcmp(value, name) != 0) {
        return 0;
    }
    char devname[INKWELL_BLOCK_PATH_MAX];
    char major[16];
    char minor[16];
    if (inkwell_block_uevent_value(text, "DEVNAME", devname, sizeof devname) != 0 ||
        inkwell_block_uevent_value(text, "MAJOR", major, sizeof major) != 0 ||
        inkwell_block_uevent_value(text, "MINOR", minor, sizeof minor) != 0 ||
        !parse_u32(major, &out->major) || !parse_u32(minor, &out->minor) ||
        (size_t)snprintf(out->path, sizeof out->path, "/dev/%s", devname) >= sizeof out->path) {
        return 0;
    }
    /* `size` is in 512-byte sectors, whatever the device's own block size. */
    snprintf(path, sizeof path, "%s/class/block/%s/size", sys, entry);
    char size[32];
    out->size = 0U;
    if (read_small(path, size, sizeof size) > 0) {
        out->size = (uint64_t)strtoull(size, NULL, 10) * 512U;
    }
    return 1;
}

int inkwell_block_find(const char *sys, const char *name, struct inkwell_block_partition *out) {
    if (sys == NULL || name == NULL || out == NULL || name[0] == '\0' ||
        strlen(name) >= INKWELL_BLOCK_NAME_MAX) {
        return -EINVAL;
    }
    char dir_path[512];
    snprintf(dir_path, sizeof dir_path, "%s/class/block", sys);
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return -errno;
    }
    int found = 0;
    struct inkwell_block_partition candidate;
    for (struct dirent *e = readdir(dir); e != NULL; e = readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        memset(&candidate, 0, sizeof candidate);
        if (match(sys, e->d_name, name, &candidate)) {
            if (++found == 1) {
                *out = candidate;
            }
        }
    }
    (void)closedir(dir);
    return found == 0 ? -ENOENT : found > 1 ? -EEXIST : 0;
}
