#include "../base/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint64_t inkwell_platform_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

bool inkwell_platform_utc_time(time_t epoch, struct tm *out) {
    return gmtime_r(&epoch, out) != NULL;
}

int inkwell_platform_file_open_rw(const char *path) {
    return open(path, O_RDWR | O_CLOEXEC);
}

ssize_t inkwell_platform_file_read_at(int fd, void *buffer, size_t count, off_t offset) {
    return pread(fd, buffer, count, offset);
}

ssize_t inkwell_platform_file_write_at(int fd, const void *buffer, size_t count, off_t offset) {
    return pwrite(fd, buffer, count, offset);
}

int inkwell_platform_file_truncate(int fd, off_t length) {
    return ftruncate(fd, length);
}

int inkwell_platform_file_close(int fd) {
    return close(fd);
}

int inkwell_platform_file_sync(int fd) {
    return fsync(fd);
}

int inkwell_platform_file_stream_fd(FILE *file) {
    return fileno(file);
}

int inkwell_platform_file_replace(const char *temp, const char *path, bool sync_data) {
    (void)sync_data;
    return rename(temp, path);
}

int inkwell_platform_parent_sync(const char *path) {
    char *parent = strdup(path);
    if (parent == NULL) {
        return -ENOMEM;
    }
    char *slash = strrchr(parent, '/');
    const char *directory = ".";
    if (slash != NULL) {
        if (slash == parent) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        directory = parent;
    }
    const int fd = open(directory, O_RDONLY);
    int result = fd < 0 ? -errno : 0;
    if (fd >= 0) {
        if (fsync(fd) != 0) {
            result = -errno;
        }
        if (close(fd) != 0 && result == 0) {
            result = -errno;
        }
    }
    free(parent);
    return result;
}

int inkwell_platform_dir_make(const char *path) {
    return mkdir(path, 0700);
}
