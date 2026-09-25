#define _POSIX_C_SOURCE 200809L

#include "inkwell/base/log.h"

#include "inkwell/base/env.h"
#include "platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static enum inkwell_log_level g_log_level = INKWELL_LOG_LEVEL_INFO;

void inkwell_log_set_level(enum inkwell_log_level level) {
    g_log_level = level;
}

enum inkwell_log_level inkwell_log_get_level(void) {
    return g_log_level;
}

const char *inkwell_log_level_to_string(enum inkwell_log_level level) {
    switch (level) {
    case INKWELL_LOG_LEVEL_TRACE:
        return "TRACE";
    case INKWELL_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case INKWELL_LOG_LEVEL_INFO:
        return "INFO";
    case INKWELL_LOG_LEVEL_WARN:
        return "WARN";
    case INKWELL_LOG_LEVEL_ERROR:
        return "ERROR";
    case INKWELL_LOG_LEVEL_NONE:
        return "NONE";
    }
    return "UNKNOWN";
}

static void format_timestamp(char *buffer, size_t buffer_len) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
        return;
    }

    struct tm tm_result;
    if (!inkwell_platform_utc_time(ts.tv_sec, &tm_result)) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
        return;
    }

    const long millis = ts.tv_nsec / 1000000L;
    const int written = snprintf(buffer, buffer_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                                 tm_result.tm_year + 1900, tm_result.tm_mon + 1, tm_result.tm_mday,
                                 tm_result.tm_hour, tm_result.tm_min, tm_result.tm_sec, millis);
    /* The same answer as the two failures above, and for the same reason: a timestamp cut off
       mid-field still reads as a time. A struct tm that gmtime_r filled in cannot overflow the
       32 bytes every caller passes - but neither the compiler nor this function knows that the
       buffer is that big or that the fields are in range. */
    if (written < 0 || (size_t)written >= buffer_len) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
    }
}

/*
 * The same line again, into the ring a crash report carries.
 *
 * A copy rather than a redirect, which is the point: stderr keeps streaming through vfprintf
 * exactly as it did, so nothing about the log on the card changes, and the ring gets a bounded
 * rendering of the same line. `va_copy` is what lets both read the arguments - a va_list is
 * consumed by the first thing that walks it, and handing the same one to two formatters is the
 * kind of bug that works on one architecture.
 *
 * It is taken *after* the level check on purpose. The ring is meant to be the tail of the log
 * the user is looking at, and a ring holding lines the log file never showed would have the
 * crash report and the log disagree about what the client did - which is worse than a ring that
 * is quiet because the level was set high.
 */
/* The longest line a sink is handed. A log line is a sentence, not a payload. */
#define INKWELL_LOG_SINK_LINE_MAX 256U

static void (*g_sink)(const char *line);

void inkwell_log_set_sink(void (*sink)(const char *line)) {
    g_sink = sink;
}

static void log_capture(const char *timestamp, enum inkwell_log_level level, const char *component,
                        const char *fmt, va_list args)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 4, 0)));

static void log_capture(const char *timestamp, enum inkwell_log_level level, const char *component,
                        const char *fmt, va_list args) {
    char line[INKWELL_LOG_SINK_LINE_MAX];
    int used =
        snprintf(line, sizeof line, "%s [%s]", timestamp, inkwell_log_level_to_string(level));
    if (used < 0) {
        return;
    }
    size_t offset = (size_t)used < sizeof line ? (size_t)used : sizeof line - 1U;

    if (component != NULL && component[0] != '\0' && offset + 2U < sizeof line) {
        used = snprintf(line + offset, sizeof line - offset, " (%s)", component);
        if (used > 0) {
            offset +=
                (size_t)used < sizeof line - offset ? (size_t)used : sizeof line - offset - 1U;
        }
    }
    if (offset + 2U < sizeof line) {
        line[offset++] = ':';
        line[offset++] = ' ';
        line[offset] = '\0';
    }
    (void)vsnprintf(line + offset, sizeof line - offset, fmt, args);

    /* The message may have ended in the newline the stderr path below adds for itself. A ring
       entry is one line by construction, so it is taken back off rather than written out as a
       blank line in the middle of the report. */
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
    if (g_sink != NULL) {
        g_sink(line);
    }
}

void inkwell_log_message_v(enum inkwell_log_level level, const char *component, const char *fmt,
                           va_list args) {
    if (level < g_log_level || level == INKWELL_LOG_LEVEL_NONE) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof timestamp);

    va_list captured;
    va_copy(captured, args);
    log_capture(timestamp, level, component, fmt, captured);
    va_end(captured);

    fprintf(stderr, "%s [%s]", timestamp, inkwell_log_level_to_string(level));
    if (component != NULL && component[0] != '\0') {
        fprintf(stderr, " (%s)", component);
    }
    fprintf(stderr, ": ");

    vfprintf(stderr, fmt, args);

    if (fmt[0] == '\0' || fmt[strlen(fmt) - 1] != '\n') {
        fputc('\n', stderr);
    }
}

/* ---- the file on the card ------------------------------------------------------------------- */

/* One step of the scan and of the shift. Small on purpose: together they cover at most
   INKWELL_LOG_FILE_KEEP_BYTES once each, at startup, and a bigger buffer would buy nothing a card's
   own readahead does not already. */
#define LOG_FILE_CHUNK 4096U

/* The start of the path component that ends at `end`, where `end` indexes its trailing '/'. */
static size_t log_file_component_start(const char *path, size_t end) {
    size_t at = end;
    while (at > 0U && path[at - 1U] != '/') {
        at--;
    }
    return at;
}

bool inkwell_log_file_default_path(char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';

    const char *override = inkwell_env_get("LOG_FILE");
    if (override != NULL && override[0] != '\0') {
        const int written = snprintf(out, out_len, "%s", override);
        if (written < 0 || (size_t)written >= out_len) {
            out[0] = '\0';
            return false;
        }
        return true;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return false;
    }

    /* `HOME` is `.userdata/$PLATFORM/<pak>`, so the pak name is its last component and the logs
       directory is its sibling. A trailing slash would otherwise make the name empty and the
       whole derivation nonsense, so it is stepped over first. */
    size_t home_len = strlen(home);
    while (home_len > 1U && home[home_len - 1U] == '/') {
        home_len--;
    }
    /* Scanned by hand rather than with memrchr(), which is a GNU extension and not declared
       under the _POSIX_C_SOURCE this file asks for - the cross build does not have it. */
    size_t split = home_len;
    while (split > 0U && home[split - 1U] != '/') {
        split--;
    }
    if (split <= 1U) {
        return false;
    }
    const size_t parent_len = split - 1U;
    const char *name = home + split;
    const size_t name_len = home_len - split;
    if (name_len == 0U) {
        return false;
    }

    /*
     * And it has to actually *be* the launcher's directory, not merely have two components.
     *
     * Without this the derivation fires on every ordinary host too: `HOME=/srv/users/alice` names
     * `/srv/users/logs/alice.txt`, and if a file happens to be sitting there this would cut back
     * somebody else's. Requiring the `.userdata/<platform>/<pak>` shape that `launch.sh` builds
     * keeps a guess at the path from ever reaching a file the client does not own. Anyone whose
     * log is somewhere else says so with <PREFIX>_LOG_FILE, which is checked above and is not
     * subject to this.
     */
    const size_t platform_end = split - 1U;
    const size_t platform_start = log_file_component_start(home, platform_end);
    if (platform_start == 0U || platform_start == platform_end) {
        return false;
    }
    static const char userdata[] = ".userdata";
    const size_t userdata_end = platform_start - 1U;
    const size_t userdata_start = log_file_component_start(home, userdata_end);
    if (userdata_end - userdata_start != sizeof userdata - 1U ||
        strncmp(home + userdata_start, userdata, sizeof userdata - 1U) != 0) {
        return false;
    }

    const int written =
        snprintf(out, out_len, "%.*s/logs/%.*s.txt", (int)parent_len, home, (int)name_len, name);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/*
 * Steps forward from `from` to just past the next newline, and answers where that is.
 *
 * Reads through the same descriptor the rest of the compaction uses, so the scan cannot land on
 * a different file from the one that was measured. -1 when there is no newline left, which means
 * a single line longer than the whole retained window.
 */
static off_t log_file_line_start(int fd, off_t from, off_t size) {
    char chunk[LOG_FILE_CHUNK];
    off_t at = from;
    while (at < size) {
        const off_t remaining = size - at;
        size_t want = sizeof chunk;
        if ((off_t)want > remaining) {
            want = (size_t)remaining;
        }
        const ssize_t got = inkwell_platform_file_read_at(fd, chunk, want, at);
        if (got <= 0) {
            return -1;
        }
        for (ssize_t i = 0; i < got; ++i) {
            if (chunk[i] == '\n') {
                return at + (off_t)i + 1;
            }
        }
        at += (off_t)got;
    }
    return -1;
}

/*
 * Shifts [`from`, `size`) down to the front of `fd` and cuts the file to what was moved.
 *
 * A read-then-write walk down the same descriptor rather than a copy through a second file: the
 * destination is always behind the source, so a forward walk never reads a byte it has already
 * overwritten. Returns the file's new length, or -errno.
 */
static off_t log_file_shift_to_front(int fd, off_t from, off_t size) {
    char chunk[LOG_FILE_CHUNK];
    off_t read_at = from;
    off_t write_at = 0;
    while (read_at < size) {
        const off_t remaining = size - read_at;
        size_t want = sizeof chunk;
        if ((off_t)want > remaining) {
            want = (size_t)remaining;
        }
        const ssize_t got = inkwell_platform_file_read_at(fd, chunk, want, read_at);
        if (got < 0) {
            return (off_t)-errno;
        }
        if (got == 0) {
            break; /* someone truncated it underneath us; what moved is what there is */
        }
        ssize_t put_total = 0;
        while (put_total < got) {
            const ssize_t put = inkwell_platform_file_write_at(
                fd, chunk + put_total, (size_t)(got - put_total), write_at + (off_t)put_total);
            if (put <= 0) {
                return (off_t)-errno;
            }
            put_total += put;
        }
        read_at += (off_t)got;
        write_at += (off_t)got;
    }
    if (inkwell_platform_file_truncate(fd, write_at) != 0) {
        return (off_t)-errno;
    }
    return write_at;
}

long inkwell_log_file_compact(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    /*
     * The name is resolved once, here, and everything after this works on the descriptor.
     *
     * It used to stat() the path and then open it twice more by name, which CodeQL reported as a
     * time-of-check/time-of-use race and was right to: the size and the file type that decide
     * whether to truncate anything were established against a name, and it is the *file* that
     * then gets truncated. One open() and an fstat() on its descriptor close that gap, and the
     * rewrite below never names the file again - which also makes keeping tee's inode structural
     * rather than something the code merely happens to do.
     */
    const int fd = inkwell_platform_file_open_rw(path);
    if (fd < 0) {
        /* Not there is not a failure: see the header on why a derived path that was never a log
           has to be a no-op rather than an error. */
        return errno == ENOENT ? 0L : -errno;
    }

    struct stat info;
    if (fstat(fd, &info) != 0) {
        const int failed = -errno;
        (void)inkwell_platform_file_close(fd);
        return failed;
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= (off_t)INKWELL_LOG_FILE_MAX_BYTES) {
        (void)inkwell_platform_file_close(fd);
        return 0L;
    }

    /* The cut is a byte count, so it lands mid-line in the ordinary case; the remainder of that
       line is dropped so the file never opens on half a sentence. */
    const off_t resume =
        log_file_line_start(fd, info.st_size - (off_t)INKWELL_LOG_FILE_KEEP_BYTES, info.st_size);
    /*
     * No newline in the retained window, or nothing after the one there was, means a single line
     * longer than the whole window. Emptying the file is the one outcome worse than leaving it
     * oversized: that line is also the only thing in it worth reading, so the cap yields rather
     * than throwing the log away.
     */
    if (resume < 0 || resume >= info.st_size) {
        (void)inkwell_platform_file_close(fd);
        return 0L;
    }

    /*
     * Anything `tee` appends between the fstat above and the ftruncate inside here is past
     * `info.st_size` and is dropped with the rest of the old file. At startup that is nothing -
     * the launch banner is already in the file and the client has barely logged - and a couple of
     * lines is the price of the trim happening at all. The previous shape lost strictly more: it
     * truncated the file to zero before writing the tail back.
     */
    const off_t shifted = log_file_shift_to_front(fd, resume, info.st_size);
    (void)inkwell_platform_file_close(fd);
    if (shifted < 0) {
        return (long)shifted;
    }
    return (long)(info.st_size - shifted);
}

void inkwell_log_file_compact_default(void) {
    char path[INKWELL_LOG_FILE_PATH_MAX];
    if (!inkwell_log_file_default_path(path, sizeof path)) {
        return;
    }
    const long reclaimed = inkwell_log_file_compact(path);
    if (reclaimed < 0) {
        inkwell_log_warn("log", "Could not cut back %s: %s", path, strerror((int)-reclaimed));
    } else if (reclaimed > 0) {
        inkwell_log_info("log", "Cut %s back to its newest %u KB (reclaimed %ld KB)", path,
                         (unsigned)(INKWELL_LOG_FILE_KEEP_BYTES / 1024U), reclaimed / 1024L);
    }
}
