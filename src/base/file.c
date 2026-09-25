#include "inkwell/base/file.h"
#include "platform.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>

uint8_t *inkwell_file_read(const char *path, size_t max_len, size_t *out_len) {
    if (path == NULL || out_len == NULL) {
        return NULL;
    }
    *out_len = 0U;
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0 || (size_t)size > max_len || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *const bytes = malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    const size_t got = fread(bytes, 1U, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *out_len = got;
    return bytes;
}

int inkwell_file_mkdir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    return inkwell_platform_dir_make(path) == 0 ? 0 : -errno;
}

int inkwell_file_replace(const char *from, const char *to) {
    if (from == NULL || from[0] == '\0' || to == NULL || to[0] == '\0') {
        return -EINVAL;
    }
    return inkwell_platform_file_replace(from, to, false) == 0 ? 0 : -errno;
}

bool inkwell_file_is_dir(const char *path) {
    return path != NULL && path[0] != '\0' && inkwell_platform_is_dir(path);
}

int inkwell_file_list(const char *dir, inkwell_file_entry_fn visit, void *context) {
    if (dir == NULL || dir[0] == '\0' || visit == NULL) {
        return -EINVAL;
    }
    return inkwell_platform_dir_list(dir, visit, context);
}
