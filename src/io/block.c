#include "inkwell/io/block.h"

#include <errno.h>
#include <string.h>

/* The uevent text parser: plain text, built everywhere. The lookup is block_sysfs.c. */

int inkwell_block_uevent_value(const char *text, const char *key, char *out, size_t size) {
    if (text == NULL || key == NULL || out == NULL || size == 0U) {
        return -EINVAL;
    }
    const size_t key_len = strlen(key);
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        const size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            const size_t value_len = len - key_len - 1U;
            if (value_len >= size) {
                return -ENOSPC;
            }
            memcpy(out, line + key_len + 1U, value_len);
            out[value_len] = '\0';
            return 0;
        }
        line += len;
        if (*line == '\n') {
            ++line;
        }
    }
    return -ENOENT;
}
