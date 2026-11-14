#include "inkwell/io/pcm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Copies the field from `start` up to " : " (or the end of the line) into `out`, trimmed. Returns
   where the next field starts, or NULL when the line ends first. */
static const char *field(const char *start, const char *line_end, char *out, size_t out_size) {
    while (start < line_end && *start == ' ') {
        ++start;
    }
    const char *sep = start;
    while (sep < line_end &&
           !(sep[0] == ' ' && sep + 2 < line_end && sep[1] == ':' && sep[2] == ' ')) {
        ++sep;
    }
    size_t len = (size_t)(sep - start);
    while (len > 0 && start[len - 1] == ' ') {
        --len;
    }
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return sep < line_end ? sep + 3 : NULL;
}

/* Reads a two-or-more digit number at `*p`, moving past it. */
static bool number(const char **p, const char *end, unsigned *out) {
    const char *s = *p;
    unsigned value = 0;
    if (s >= end || *s < '0' || *s > '9') {
        return false;
    }
    while (s < end && *s >= '0' && *s <= '9') {
        value = value * 10U + (unsigned)(*s - '0');
        if (value > 999U) {
            return false;
        }
        ++s;
    }
    *p = s;
    *out = value;
    return true;
}

/* True when the rest of the line names a playback stream: a ": playback N" field. */
static bool has_playback(const char *rest, const char *line_end) {
    static const char word[] = ": playback ";
    const size_t len = sizeof word - 1;
    for (const char *s = rest - 2; s + len <= line_end; ++s) {
        if (memcmp(s, word, len) == 0) {
            return true;
        }
    }
    return false;
}

int inkwell_pcm_parse_list(const char *text, struct inkwell_pcm_device *out, size_t max) {
    if (text == NULL || (out == NULL && max > 0)) {
        return -EINVAL;
    }
    size_t found = 0;
    for (const char *line = text; *line != '\0' && found < max;) {
        const char *line_end = strchr(line, '\n');
        if (line_end == NULL) {
            line_end = line + strlen(line);
        }
        /* "CC-DD: id : name : playback 1 : capture 1" */
        const char *p = line;
        struct inkwell_pcm_device device;
        memset(&device, 0, sizeof device);
        if (number(&p, line_end, &device.card) && p < line_end && *p == '-' && (++p, true) &&
            number(&p, line_end, &device.device) && p < line_end && *p == ':') {
            const char *next = field(p + 1, line_end, device.id, sizeof device.id);
            if (next != NULL) {
                next = field(next, line_end, device.name, sizeof device.name);
                if (next != NULL && has_playback(next, line_end)) {
                    out[found++] = device;
                }
            }
        }
        line = *line_end == '\n' ? line_end + 1 : line_end;
    }
    return (int)found;
}

int inkwell_pcm_fd(const struct inkwell_pcm *pcm) {
    return pcm == NULL ? -1 : pcm->fd;
}
