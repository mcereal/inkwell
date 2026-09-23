#define _POSIX_C_SOURCE 200809L
#include "inkwell/base/record_file.h"
#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int inkwell_record_read(FILE *file, char *line, size_t capacity, inkwell_record_visit_fn visit,
                        void *context) {
    if (file == NULL || line == NULL || capacity < 2U || visit == NULL || capacity > INT32_MAX) {
        return -EINVAL;
    }
    while (fgets(line, (int)capacity, file) != NULL) {
        const size_t length = strlen(line);
        bool complete = length > 0U && line[length - 1U] == '\n';
        if (!complete && length == capacity - 1U) {
            /* The newline itself may be the first byte beyond the caller's buffer. */
            int next = fgetc(file);
            complete = next == '\n';
            if (!complete && next != EOF) {
                while ((next = fgetc(file)) != '\n' && next != EOF) {
                }
            }
        }
        if (!complete) {
            continue; /* an overlong line or an interrupted final append */
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        char *value = equals + 1;
        inkwell_record_unescape(value);
        visit(context, line, value);
    }
    return ferror(file) ? -EIO : 0;
}

void inkwell_record_write_escaped(FILE *file, const char *value) {
    if (file == NULL || value == NULL) {
        return;
    }
    for (const unsigned char *ptr = (const unsigned char *)value; *ptr != '\0'; ++ptr) {
        if (*ptr < 0x20U || *ptr == '\\' || *ptr == '=') {
            fprintf(file, "\\x%02x", *ptr);
        } else {
            fputc((int)*ptr, file);
        }
    }
}

void inkwell_record_unescape(char *value) {
    if (value == NULL) {
        return;
    }
    char *write_ptr = value;
    for (char *read_ptr = value; *read_ptr != '\0'; ++read_ptr) {
        if (*read_ptr == '\\') {
            if (read_ptr[1] == 'x' && read_ptr[2] != '\0' && read_ptr[3] != '\0') {
                char hex[3] = {read_ptr[2], read_ptr[3], '\0'};
                *write_ptr++ = (char)strtol(hex, NULL, 16);
                read_ptr += 3;
            }
        } else {
            *write_ptr++ = *read_ptr;
        }
    }
    *write_ptr = '\0';
}

int inkwell_record_replace(const char *path, char *temp, size_t temp_capacity,
                           inkwell_record_write_fn write_records, void *context, bool sync_data) {
    if (path == NULL || path[0] == '\0' || temp == NULL || temp_capacity == 0U ||
        write_records == NULL) {
        return -EINVAL;
    }
    const int named = snprintf(temp, temp_capacity, "%s.tmp", path);
    if (named < 0 || (size_t)named >= temp_capacity) {
        return -ENAMETOOLONG;
    }
    FILE *file = fopen(temp, "w");
    if (file == NULL) {
        return -errno;
    }
    write_records(file, context);
    int result = ferror(file) ? -EIO : 0;
    if (result == 0 && sync_data && fflush(file) != 0) {
        result = -errno;
    }
    if (result == 0 && sync_data &&
        inkwell_platform_file_sync(inkwell_platform_file_stream_fd(file)) != 0) {
        result = -errno;
    }
    if (fclose(file) != 0 && result == 0) {
        result = -errno;
    }
    if (result == 0 && inkwell_platform_file_replace(temp, path, sync_data) != 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)remove(temp);
        return result;
    }
    return sync_data ? inkwell_platform_parent_sync(path) : 0;
}

int inkwell_record_append(const char *path, inkwell_record_write_fn write_records, void *context) {
    if (path == NULL || path[0] == '\0' || write_records == NULL) {
        return -EINVAL;
    }
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return -errno;
    }
    write_records(file, context);
    int result = ferror(file) ? -EIO : 0;
    if (fclose(file) != 0 && result == 0) {
        result = -errno;
    }
    return result;
}
