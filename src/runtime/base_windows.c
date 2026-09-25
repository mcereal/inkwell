#include "../base/platform.h"

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <windows.h>

uint64_t inkwell_platform_monotonic_ms(void) {
    return (uint64_t)GetTickCount64();
}

bool inkwell_platform_utc_time(time_t epoch, struct tm *out) {
    return gmtime_s(out, &epoch) == 0;
}

int inkwell_platform_file_open_rw(const char *path) {
    return _open(path, _O_RDWR | _O_BINARY | _O_NOINHERIT);
}

ssize_t inkwell_platform_file_read_at(int fd, void *buffer, size_t count, off_t offset) {
    if (_lseeki64(fd, (__int64)offset, SEEK_SET) < 0) {
        return -1;
    }
    return (ssize_t)_read(fd, buffer, (unsigned)count);
}

ssize_t inkwell_platform_file_write_at(int fd, const void *buffer, size_t count, off_t offset) {
    if (_lseeki64(fd, (__int64)offset, SEEK_SET) < 0) {
        return -1;
    }
    return (ssize_t)_write(fd, buffer, (unsigned)count);
}

int inkwell_platform_file_truncate(int fd, off_t length) {
    const errno_t error = _chsize_s(fd, (__int64)length);
    if (error != 0) {
        errno = (int)error;
        return -1;
    }
    return 0;
}

int inkwell_platform_file_close(int fd) {
    return _close(fd);
}

int inkwell_platform_file_sync(int fd) {
    return _commit(fd);
}

int inkwell_platform_file_stream_fd(FILE *file) {
    return _fileno(file);
}

int inkwell_platform_file_replace(const char *temp, const char *path, bool sync_data) {
    const DWORD flags = MOVEFILE_REPLACE_EXISTING | (sync_data ? MOVEFILE_WRITE_THROUGH : 0U);
    if (MoveFileExA(temp, path, flags) == 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int inkwell_platform_parent_sync(const char *path) {
    (void)path;
    return 0;
}

int inkwell_platform_dir_make(const char *path) {
    return _mkdir(path);
}
