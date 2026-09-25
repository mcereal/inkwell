#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

/* OS services used by the base leaves. Implementations live in runtime/. */
uint64_t inkwell_platform_monotonic_ms(void);
bool inkwell_platform_utc_time(time_t epoch, struct tm *out);
int inkwell_platform_file_open_rw(const char *path);
ssize_t inkwell_platform_file_read_at(int fd, void *buffer, size_t count, off_t offset);
ssize_t inkwell_platform_file_write_at(int fd, const void *buffer, size_t count, off_t offset);
int inkwell_platform_file_truncate(int fd, off_t length);
int inkwell_platform_file_close(int fd);
int inkwell_platform_file_sync(int fd);
int inkwell_platform_file_stream_fd(FILE *file);
int inkwell_platform_file_replace(const char *temp, const char *path, bool sync_data);
int inkwell_platform_parent_sync(const char *path);
int inkwell_platform_dir_make(const char *path);
