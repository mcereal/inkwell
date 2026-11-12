#include "inkwell/io/uevent.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The value after "KEY=" when `entry` is that key's, or NULL. */
static const char *value_of(const char *entry, const char *key, size_t key_len) {
    return strncmp(entry, key, key_len) == 0 && entry[key_len] == '=' ? entry + key_len + 1 : NULL;
}

static bool parse_u64(const char *text, uint64_t *out) {
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    const int saved = errno;
    errno = 0;
    char *end = NULL;
    const unsigned long long value = strtoull(text, &end, 10);
    const bool ok = errno == 0 && end != NULL && *end == '\0';
    errno = saved;
    if (ok) {
        *out = (uint64_t)value;
    }
    return ok;
}

int inkwell_uevent_parse(const char *buf, size_t len, struct inkwell_uevent *out) {
    if (buf == NULL || out == NULL || len == 0 || buf[len - 1] != '\0') {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    /* "action@devpath": the one header the kernel writes. udevd's begins "libudev\0" and is
       binary after it, so it has no '@' in its first string and stops here. */
    const char *header = buf;
    const size_t header_len = strlen(header);
    const char *at = strchr(header, '@');
    if (at == NULL || at == header || at[1] != '/') {
        return -EINVAL;
    }
    if (header_len + 1 >= len) {
        return -EINVAL; /* a header and nothing after it */
    }

    out->env = buf + header_len + 1;
    out->env_len = len - header_len - 1;
    for (const char *entry = out->env; entry < buf + len; entry += strlen(entry) + 1) {
        const char *value;
        if ((value = value_of(entry, "ACTION", 6)) != NULL) {
            out->action = value;
        } else if ((value = value_of(entry, "DEVPATH", 7)) != NULL) {
            out->devpath = value;
        } else if ((value = value_of(entry, "SUBSYSTEM", 9)) != NULL) {
            out->subsystem = value;
        } else if ((value = value_of(entry, "DEVNAME", 7)) != NULL) {
            out->devname = value;
        } else if ((value = value_of(entry, "DEVTYPE", 7)) != NULL) {
            out->devtype = value;
        } else if ((value = value_of(entry, "SEQNUM", 6)) != NULL) {
            if (!parse_u64(value, &out->seqnum)) {
                memset(out, 0, sizeof *out);
                return -EINVAL;
            }
        }
    }

    /* The header repeats ACTION and DEVPATH; one that disagrees is not the kernel's. */
    const size_t action_len = (size_t)(at - header);
    if (out->action == NULL || out->devpath == NULL || strlen(out->action) != action_len ||
        strncmp(out->action, header, action_len) != 0 || strcmp(out->devpath, at + 1) != 0) {
        memset(out, 0, sizeof *out);
        return -EINVAL;
    }
    return 0;
}

const char *inkwell_uevent_get(const struct inkwell_uevent *event, const char *key) {
    if (event == NULL || key == NULL || event->env == NULL) {
        return NULL;
    }
    const size_t key_len = strlen(key);
    const char *end = event->env + event->env_len;
    for (const char *entry = event->env; entry < end; entry += strlen(entry) + 1) {
        const char *value = value_of(entry, key, key_len);
        if (value != NULL) {
            return value;
        }
    }
    return NULL;
}
