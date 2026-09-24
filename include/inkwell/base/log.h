#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * The format archetype every printf-like declaration here is checked against.
 *
 * On MinGW GCC plain `printf` means MSVCRT's dialect, which has no `%zu` - so every size_t in a
 * log line was a -Wformat warning on a Windows build that prints it correctly. <stdio.h> names
 * the dialect the linked CRT actually speaks: `gnu_printf` under UCRT, `ms_printf` under MSVCRT.
 */
#if defined(__MINGW_PRINTF_FORMAT)
#define INKWELL_PRINTF_ARCHETYPE __MINGW_PRINTF_FORMAT
#else
#define INKWELL_PRINTF_ARCHETYPE printf
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a copy of every line that passes the level check also goes.
 *
 * inkwell writes the log; what else wants a copy is the application's business. A crash
 * reporter keeping the last few lines is the case this exists for, and it is a sink rather than
 * something inkwell calls by name because a UI library has no business knowing whether the app
 * it is drawing for writes crash reports at all.
 *
 * Called with one line, already formatted and with no trailing newline. NULL removes the sink.
 * Taken *after* the level check on purpose: a sink holding lines the log file never showed
 * would have the two disagree about what the application did.
 */
void inkwell_log_set_sink(void (*sink)(const char *line));

enum inkwell_log_level {
    INKWELL_LOG_LEVEL_TRACE = 0,
    INKWELL_LOG_LEVEL_DEBUG,
    INKWELL_LOG_LEVEL_INFO,
    INKWELL_LOG_LEVEL_WARN,
    INKWELL_LOG_LEVEL_ERROR,
    INKWELL_LOG_LEVEL_NONE
};

void inkwell_log_set_level(enum inkwell_log_level level);
enum inkwell_log_level inkwell_log_get_level(void);
const char *inkwell_log_level_to_string(enum inkwell_log_level level);

/*
 * The archetype is printf's and the argument index is 0, which is how the attribute spells a
 * va_list variant: there are no further arguments here to check `fmt` against, and saying so is
 * what tells the compiler this format *is* a parameter rather than a string assembled somewhere
 * it cannot see. Without it the vfprintf() inside is a -Wformat-nonliteral on every clang build.
 */
void inkwell_log_message_v(enum inkwell_log_level level, const char *component, const char *fmt,
                           va_list args) __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 3, 0)));

static inline void inkwell_log_trace(const char *component, const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 2, 3)));
static inline void inkwell_log_debug(const char *component, const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 2, 3)));
static inline void inkwell_log_info(const char *component, const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 2, 3)));
static inline void inkwell_log_warn(const char *component, const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 2, 3)));
static inline void inkwell_log_error(const char *component, const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 2, 3)));

static inline void inkwell_log_trace(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    inkwell_log_message_v(INKWELL_LOG_LEVEL_TRACE, component, fmt, args);
    va_end(args);
}

static inline void inkwell_log_debug(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    inkwell_log_message_v(INKWELL_LOG_LEVEL_DEBUG, component, fmt, args);
    va_end(args);
}

static inline void inkwell_log_info(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    inkwell_log_message_v(INKWELL_LOG_LEVEL_INFO, component, fmt, args);
    va_end(args);
}

static inline void inkwell_log_warn(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    inkwell_log_message_v(INKWELL_LOG_LEVEL_WARN, component, fmt, args);
    va_end(args);
}

static inline void inkwell_log_error(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    inkwell_log_message_v(INKWELL_LOG_LEVEL_ERROR, component, fmt, args);
    va_end(args);
}

/* ---- the file on the card ------------------------------------------------------------------- */

/*
 * Everything above writes to `stderr` and nothing here owns a file. On device it is `launch.sh`
 * that gives the stream somewhere to land, by piping the client through `tee -a` into
 * `/.userdata/$PLATFORM/logs/<pak>.txt` - an append, every run, with nothing ever cutting it
 * back. The other files an application writes to the card can all compact themselves - its own
 * caches cap themselves, and a crash report carries a bounded ring - but the log is the one that
 * grows without a limit, because it is owned by a shell script rather than by any of this.
 *
 * So the process cuts it back itself, at startup.
 *
 * **In place, and deliberately not by renaming.** `tee` already holds the file open by the time
 * `main()` runs, and its descriptor follows the inode: rename the file to `.1` and the whole of
 * *this* run goes into the rotated copy while the live path stays empty. Rewriting the same inode
 * is what keeps `tee` correct instead - it appends with `O_APPEND`, so its next write positions
 * itself at the new, shorter end and nothing is lost and nothing is written into a hole.
 *
 * The rewrite opens the path once and works on that descriptor from then on: the tail is shifted
 * down the file itself and the file is cut to what moved. Nothing temporary is written beside the
 * log, and the size that decides whether to truncate is read from the same descriptor that gets
 * truncated - measuring one file by name and then truncating whatever the name points at later is
 * the race CodeQL reports, and keeping tee's inode stops being luck once the name is out of it.
 *
 * Startup is the only moment this runs. A single session is left to grow, which is the trade:
 * what made the file unbounded was accumulating across every run since the card was written, and
 * mid-run truncation would be cutting the file underneath a `tee` that is actively writing a
 * line into it.
 */

/* When the log is cut back, and how much of the tail survives it.
 *
 * The threshold sits well above what is kept, for the reason any such cap does:
 * a file that came back from a compaction already close to tripping it would be rewritten again
 * on the next launch, and a launch is not a rare event. 128 KB of ordinary log lines is on the
 * order of fifteen hundred of them - far more than the 32 a crash report carries, and enough to
 * read what the process was doing before whatever is being investigated. */
#define INKWELL_LOG_FILE_MAX_BYTES (512U * 1024U)
#define INKWELL_LOG_FILE_KEEP_BYTES (128U * 1024U)

/* Enough for the logs directory plus an application name and the extension: a path here is a
   value, not an allocation. */
#define INKWELL_LOG_FILE_PATH_MAX 256U

/*
 * Where `launch.sh` puts the log, worked out without its help.
 *
 * `<PREFIX>_LOG_FILE` answers directly when it is set. Otherwise the path is derived from
 * `HOME`, which the launcher points at `.userdata/$PLATFORM/<pak>`: the log is that directory's
 * sibling `logs/<pak>.txt`. Deriving it rather than being told is the whole point - `launch.sh`
 * does not ship through self-update and the bare binary does, so a launcher that has never heard
 * of any of this still gets its log bounded on the next update.
 *
 * **`HOME` must have that exact shape for anything to be derived.** The derivation is two
 * components of guesswork, and on an ordinary host it lands somewhere real: `HOME=/srv/users/alice`
 * would name `/srv/users/logs/alice.txt` and the cap would then cut back a file the process has
 * nothing to do with. So the `.userdata/<platform>/<pak>` shape is required, and the override
 * above - which is somebody saying where their log is - is not subject to it.
 *
 * False when there is no `HOME`, or it is not a pak's userdata directory, leaving `out` empty.
 */
bool inkwell_log_file_default_path(char *out, size_t out_len);

/*
 * Cut `path` back to its newest INKWELL_LOG_FILE_KEEP_BYTES when it has outgrown the cap.
 *
 * Returns the number of bytes reclaimed, 0 when the file was already small enough or is not
 * there, or -errno. **A file that does not exist is not an error and is not created** - which is
 * what makes a wrong guess at the path harmless: off device `HOME` derives somewhere that was
 * never a log, and the answer is that there is nothing to do rather than a stray file.
 *
 * The retained tail always starts at a line boundary, so the file never begins mid-sentence.
 */
long inkwell_log_file_compact(const char *path);

/* inkwell_log_file_default_path() and then inkwell_log_file_compact(), which is all a startup
   wants. Reports what it did through the log itself, so the reason a log begins where it does is in
   the file the reader is already holding. */
void inkwell_log_file_compact_default(void);

#ifdef __cplusplus
}
#endif
