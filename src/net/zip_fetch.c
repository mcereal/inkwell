#define _POSIX_C_SOURCE 200809L

#include "inkwell/net/zip_fetch.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

#include "inkwell/codec/inflate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What the staged files are called when the caller names no stem. */
#define ZIP_STEM_DEFAULT "zip_fetch"

/* The staged files, each "<stem>.<suffix>". Named rather than made unique because a download
   is one at a time and a leftover from a run that died is a file the next run overwrites rather
   than trips over. */
#define ZIP_FILE_WINDOW "window"
#define ZIP_FILE_CENTRAL "central"
#define ZIP_FILE_HEADER "header"
#define ZIP_FILE_MEMBER "member"
#define ZIP_FILE_IMAGE "image"

static void zip_step_window(struct inkwell_zip_fetch *zip);
static void zip_step_central(struct inkwell_zip_fetch *zip);
static void zip_step_header(struct inkwell_zip_fetch *zip);
static void zip_step_member(struct inkwell_zip_fetch *zip);
static void zip_step_inflate(struct inkwell_zip_fetch *zip);

/* ---- staging ----------------------------------------------------------------------------- */

static bool zip_path(const struct inkwell_zip_fetch *zip, const char *name,
                          char *out, size_t out_len) {
    const int written = snprintf(out, out_len, "%s/%s.%s", zip->staging, zip->stem, name);
    return written > 0 && (size_t)written < out_len;
}

static void zip_remove(const struct inkwell_zip_fetch *zip, const char *name) {
    char path[INKWELL_FETCH_PATH_MAX];
    if (zip_path(zip, name, path, sizeof path)) {
        (void)unlink(path);
    }
}

/* Everything except the result itself. Called on the way to READY and on the way to FAILED:
   the intermediates are worth nothing to a retry, and the member is the one big one. */
static void zip_clean(const struct inkwell_zip_fetch *zip) {
    zip_remove(zip, ZIP_FILE_WINDOW);
    zip_remove(zip, ZIP_FILE_CENTRAL);
    zip_remove(zip, ZIP_FILE_HEADER);
    zip_remove(zip, ZIP_FILE_MEMBER);
}

/* Reads a staged file whole. The largest is the member, which the caller's limit bounds, and
   the inflate wants all of it in memory at once anyway. */
static uint8_t *zip_read(const struct inkwell_zip_fetch *zip, const char *name,
                              size_t *out_len) {
    *out_len = 0U;
    char path[INKWELL_FETCH_PATH_MAX];
    if (!zip_path(zip, name, path, sizeof path)) {
        return NULL;
    }
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
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

static uint64_t zip_file_size(const struct inkwell_zip_fetch *zip,
                                   const char *name) {
    char path[INKWELL_FETCH_PATH_MAX];
    struct stat info;
    if (!zip_path(zip, name, path, sizeof path) || stat(path, &info) != 0) {
        return 0U;
    }
    return info.st_size > 0 ? (uint64_t)info.st_size : 0U;
}

/* ---- finishing --------------------------------------------------------------------------- */

static void zip_finish(struct inkwell_zip_fetch *zip,
                            enum inkwell_zip_fetch_state state,
                            enum inkwell_zip_fetch_error error) {
    zip->state = state;
    zip->error = error;
    zip_clean(zip);
    if (state == INKWELL_ZIP_FETCH_FAILED) {
        /* A half-written result is worse than none: it is the right length often enough to be
           tried, and it is the file a retry would otherwise skip fetching. */
        zip_remove(zip, ZIP_FILE_IMAGE);
    }
    if (zip->on_done != NULL) {
        const inkwell_zip_fetch_done_fn done = zip->on_done;
        void *const userdata = zip->userdata;
        /* Cleared before the call: a caller starting the next zip from inside this one is
           the shape inkwell_fetch already supports, and it must not see a stale callback. */
        zip->on_done = NULL;
        zip->userdata = NULL;
        done(userdata, zip);
    }
}

static void zip_fail(struct inkwell_zip_fetch *zip,
                          enum inkwell_zip_fetch_error error) {
    zip_finish(zip, INKWELL_ZIP_FETCH_FAILED, error);
}

/* ---- the range requests -------------------------------------------------------------------*/

static void zip_on_fetch(void *userdata, const struct inkwell_fetch_result *result);

/*
 * Starts one range read into `name`.
 *
 * `first` and `last` are inclusive, as HTTP means them, and they are always both present: a
 * suffix range (`bytes=-65536`) is answered `501 Unsupported client range` by at least one CDN
 * that serves large archives, which is the whole reason the HEAD step exists.
 */
static bool zip_range(struct inkwell_zip_fetch *zip, const char *name,
                           uint64_t first, uint64_t last, uint32_t timeout_ms) {
    if (!zip_path(zip, name, zip->active_path, sizeof zip->active_path)) {
        return false;
    }
    const int written = snprintf(zip->range, sizeof zip->range, "Range: bytes=%llu-%llu",
                                 (unsigned long long)first, (unsigned long long)last);
    if (written <= 0 || (size_t)written >= sizeof zip->range) {
        return false;
    }
    struct inkwell_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = zip->url;
    request.headers[0] = zip->range;
    request.output_path = zip->active_path;
    request.timeout_ms = timeout_ms;
    request.on_done = zip_on_fetch;
    request.userdata = zip;
    return inkwell_fetch_start(zip->fetch, &request, inkwell_time_monotonic_ms()) == 0;
}

static void zip_step_window(struct inkwell_zip_fetch *zip) {
    /*
     * The largest tail a conforming zip can need, or the whole file when it is smaller than
     * that - which no real archive is, but a 404 page dressed as one might be.
     */
    const uint64_t want = (uint64_t)INKWELL_ZIP_TAIL_WINDOW;
    zip->window_offset = zip->zip_size > want ? zip->zip_size - want : 0U;
    zip->window_len = (size_t)(zip->zip_size - zip->window_offset);
    zip->state = INKWELL_ZIP_FETCH_READING;
    if (!zip_range(zip, ZIP_FILE_WINDOW, zip->window_offset,
                        zip->zip_size - 1U, zip->step_timeout_ms)) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
    }
}

static void zip_step_central(struct inkwell_zip_fetch *zip) {
    zip->state = INKWELL_ZIP_FETCH_READING;
    zip->directory_only = true;
    if (!zip_range(zip, ZIP_FILE_CENTRAL, zip->window_offset,
                        zip->window_offset + (uint64_t)zip->window_len - 1U,
                        zip->step_timeout_ms)) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
    }
}

static void zip_step_header(struct inkwell_zip_fetch *zip) {
    zip->state = INKWELL_ZIP_FETCH_LOCATING;
    if (!zip_range(zip, ZIP_FILE_HEADER, zip->entry.local_header_offset,
                        zip->entry.local_header_offset + INKWELL_ZIP_LOCAL_HEADER_SIZE - 1U,
                        zip->step_timeout_ms)) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
    }
}

static void zip_step_member(struct inkwell_zip_fetch *zip) {
    zip->state = INKWELL_ZIP_FETCH_FETCHING;
    if (!zip_range(zip, ZIP_FILE_MEMBER, zip->data_offset,
                        zip->data_offset + zip->entry.compressed_size - 1U,
                        zip->member_timeout_ms)) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
    }
}

/* ---- reading what came back ---------------------------------------------------------------*/

/* The tail window, or the directory fetched on its own. Both end here. */
static void zip_read_directory(struct inkwell_zip_fetch *zip, const char *name,
                                    bool second_pass) {
    size_t len = 0U;
    uint8_t *const window = zip_read(zip, name, &len);
    if (window == NULL) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
        return;
    }

    const uint8_t *central = NULL;
    uint32_t central_size = 0U;
    uint32_t entries = 0U;
    if (second_pass) {
        /* This read *is* the directory: its bounds were taken from the record last time. */
        central = window;
        central_size = (uint32_t)len;
        entries = (uint32_t)UINT16_MAX;
    } else {
        struct inkwell_zip_end end;
        if (!inkwell_zip_find_end(window, len, zip->window_offset, &end)) {
            free(window);
            zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP);
            return;
        }
        central = inkwell_zip_central_slice(&end, window, len, zip->window_offset);
        central_size = end.central_size;
        entries = end.entries;
        zip->central_offset = end.central_offset;
        if (central == NULL) {
            /*
             * A directory bigger than the window. No archive this was measured against needs
             * this - 16 KB and 21 KB against a 64 KB window - and it is written anyway because
             * the alternative is a feature that stops working on the version that crosses the
             * line, in a way nobody would connect to the zip having grown.
             */
            zip->window_offset = end.central_offset;
            zip->window_len = (size_t)end.central_size;
            zip->central_offset = end.central_offset;
            free(window);
            inkwell_log_info("zip_fetch", "Central directory is %u bytes; fetching it on its own",
                             (unsigned)end.central_size);
            zip_step_central(zip);
            return;
        }
    }

    const enum inkwell_zip_search search =
        inkwell_zip_find_member(central, central_size, entries, zip->member, &zip->entry);
    free(window);
    if (search == INKWELL_ZIP_MALFORMED) {
        /*
         * Deliberately not NO_MEMBER. The walk stopped being a walk, which says nothing about
         * whether the file we want is in there - and NO_MEMBER is the row that tells somebody
         * upstream no longer builds for their board, which is a sentence about a different
         * thing and one they cannot retry their way out of.
         */
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP);
        return;
    }
    if (search != INKWELL_ZIP_FOUND) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NO_MEMBER);
        return;
    }
    if (zip->entry.method != INKWELL_ZIP_METHOD_DEFLATE &&
        zip->entry.method != INKWELL_ZIP_METHOD_STORE) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_UNSUPPORTED);
        return;
    }
    /*
     * The member has to sit in front of the directory that described it. Nothing in the reader
     * can check that - it is handed a directory and not a file - and the offset is the one
     * answer here that becomes a range request, so a member claiming to live inside the central
     * directory would have us fetch the directory and inflate it as the member.
     */
    if (zip->central_offset != 0U &&
        zip->entry.local_header_offset + (uint64_t)zip->entry.compressed_size >
            zip->central_offset) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP);
        return;
    }
    if (zip->entry.compressed_size == 0U || zip->entry.uncompressed_size == 0U) {
        /* Both come from the central directory, so zero here is not the 2.8.0 local-header
           case - it is a member with nothing in it, which no caller asked for. */
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NO_MEMBER);
        return;
    }
    if (zip->entry.compressed_size > zip->max_member_bytes ||
        zip->entry.uncompressed_size > zip->max_member_bytes) {
        inkwell_log_error("zip_fetch", "%s claims %u bytes in the zip and %u out; refusing it",
                          zip->entry.name, (unsigned)zip->entry.compressed_size,
                          (unsigned)zip->entry.uncompressed_size);
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_UNSUPPORTED);
        return;
    }
    zip->located = true;
    inkwell_log_info("zip_fetch", "%s is %u bytes in the zip, %u out", zip->entry.name,
                     (unsigned)zip->entry.compressed_size,
                     (unsigned)zip->entry.uncompressed_size);
    zip_step_header(zip);
}

static void zip_read_header(struct inkwell_zip_fetch *zip) {
    size_t len = 0U;
    uint8_t *const header = zip_read(zip, ZIP_FILE_HEADER, &len);
    if (header == NULL) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
        return;
    }
    const bool placed =
        inkwell_zip_local_data_start(header, len, &zip->entry, &zip->data_offset);
    free(header);
    if (!placed) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP);
        return;
    }
    zip_step_member(zip);
}

static void zip_on_fetch(void *userdata, const struct inkwell_fetch_result *result) {
    struct inkwell_zip_fetch *const zip = (struct inkwell_zip_fetch *)userdata;
    if (result->outcome != INKWELL_FETCH_OK) {
        inkwell_log_error("zip_fetch", "A range read failed (outcome %d, status %d)",
                          (int)result->outcome, result->status);
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NETWORK);
        return;
    }
    switch (zip->state) {
    case INKWELL_ZIP_FETCH_MEASURING: {
        uint64_t size = 0U;
        if (!inkwell_fetch_content_length(result->body, result->len, &size) ||
            size < INKWELL_ZIP_LOCAL_HEADER_SIZE) {
            zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP);
            return;
        }
        zip->zip_size = size;
        inkwell_log_info("zip_fetch", "The zip is %llu bytes", (unsigned long long)size);
        zip_step_window(zip);
        break;
    }
    case INKWELL_ZIP_FETCH_READING:
        /* Which of the two reads this was is the file it landed in, and the second one
           only ever happens after the first has moved `window_offset` onto the record's
           own answer. */
        zip_read_directory(
            zip, zip->directory_only ? ZIP_FILE_CENTRAL : ZIP_FILE_WINDOW,
            zip->directory_only);
        break;
    case INKWELL_ZIP_FETCH_LOCATING:
        zip_read_header(zip);
        break;
    case INKWELL_ZIP_FETCH_FETCHING:
        zip_step_inflate(zip);
        break;
    default:
        break;
    }
}

/* ---- the inflate --------------------------------------------------------------------------*/

static void zip_step_inflate(struct inkwell_zip_fetch *zip) {
    /* The work is the next tick's, so the burst of inflating happens outside the fetcher's
       completion and the done callback arrives from the tick. */
    zip->state = INKWELL_ZIP_FETCH_INFLATING;
    zip->inflate_pending = true;
}

static bool zip_write_output(const struct inkwell_zip_fetch *zip,
                                 const uint8_t *bytes, size_t len) {
    char path[INKWELL_FETCH_PATH_MAX];
    if (!zip_path(zip, ZIP_FILE_IMAGE, path, sizeof path)) {
        return false;
    }
    FILE *const file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    const bool written = fwrite(bytes, 1U, len, file) == len;
    return fclose(file) == 0 && written;
}

/*
 * The member, inflated in memory and checked against the central directory.
 *
 * The length and the CRC32 the directory carried are the zip's integrity check: there is
 * no separate verify step because this *is* one. Both come from the **central** directory,
 * never the local header, which at 2.8.0 says zero for all three - see inkwell/codec/zip.h.
 *
 * A stored member takes the same check with the inflate skipped, so there is one path to the
 * CRC rather than two, and an archive that stops compressing a member that does not compress
 * still downloads.
 */
static void zip_inflate(struct inkwell_zip_fetch *zip) {
    size_t len = 0U;
    uint8_t *const member = zip_read(zip, ZIP_FILE_MEMBER, &len);
    if (member == NULL) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
        return;
    }
    if (len != (size_t)zip->entry.compressed_size) {
        /*
         * Not a staging failure and not a corrupt member: the bytes the directory promised did
         * not all arrive. The fetcher holds a reply to its own Content-Length, and a server
         * that answered the range with a shorter one than asked for is consistent with itself,
         * so this is the only place that is caught - and it is the network's fault, which means
         * the answer is "try again" rather than "give up".
         */
        inkwell_log_error("zip_fetch", "The member arrived %zu bytes long, not %u", len,
                          (unsigned)zip->entry.compressed_size);
        free(member);
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_NETWORK);
        return;
    }

    const size_t want = (size_t)zip->entry.uncompressed_size;
    uint8_t *output = member;
    size_t produced = len;
    if (zip->entry.method == INKWELL_ZIP_METHOD_DEFLATE) {
        output = malloc(want);
        if (output == NULL) {
            free(member);
            zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
            return;
        }
        const enum inkwell_inflate_result inflated =
            inkwell_inflate(member, len, output, want, &produced);
        free(member);
        if (inflated == INKWELL_INFLATE_NO_MEMORY) {
            free(output);
            zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
            return;
        }
        if (inflated != INKWELL_INFLATE_OK) {
            inkwell_log_error("zip_fetch", "The member is not a deflate stream that fits %zu bytes",
                              want);
            free(output);
            zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_INFLATE);
            return;
        }
    }

    uint32_t crc = 0U;
    if (!inkwell_crc32(output, produced, &crc)) {
        free(output);
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
        return;
    }
    if (produced != want || crc != zip->entry.crc32) {
        /* The bytes arrived and they are not the bytes the central directory described. */
        inkwell_log_error("zip_fetch", "The member is %zu bytes with CRC %08x, not %zu with %08x",
                          produced, (unsigned)crc, want, (unsigned)zip->entry.crc32);
        free(output);
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_INFLATE);
        return;
    }

    const bool written = zip_write_output(zip, output, produced);
    free(output);
    if (!written) {
        zip_fail(zip, INKWELL_ZIP_FETCH_ERROR_STAGING);
        return;
    }
    inkwell_log_info("zip_fetch", "Staged %zu bytes of %s", produced, zip->member);
    zip_finish(zip, INKWELL_ZIP_FETCH_READY, INKWELL_ZIP_FETCH_ERROR_NONE);
}

/* ---- the public half ----------------------------------------------------------------------*/

int inkwell_zip_fetch_start(struct inkwell_zip_fetch *zip, struct inkwell_fetch *fetch,
                            const struct inkwell_zip_fetch_request *request) {
    if (zip == NULL || fetch == NULL || request == NULL || request->url == NULL ||
        request->member == NULL || request->staging_dir == NULL || request->url[0] == '\0' ||
        request->member[0] == '\0' || request->staging_dir[0] == '\0' ||
        request->max_member_bytes == 0U) {
        return -EINVAL;
    }
    const char *const stem =
        request->stem != NULL && request->stem[0] != '\0' ? request->stem : ZIP_STEM_DEFAULT;
    if (strlen(request->url) >= sizeof zip->url || strlen(request->member) >= sizeof zip->member ||
        strlen(request->staging_dir) >= sizeof zip->staging || strlen(stem) >= sizeof zip->stem) {
        /* Truncated, any of these names a different file or a different place. */
        return -EINVAL;
    }
    if (inkwell_zip_fetch_busy(zip)) {
        return -EBUSY;
    }
    if (!inkwell_fetch_available(fetch)) {
        return -ENOTSUP;
    }
    if (inkwell_fetch_busy(fetch)) {
        return -EBUSY;
    }

    memset(zip, 0, sizeof *zip);
    zip->fetch = fetch;
    inkwell_str_copy(zip->url, sizeof zip->url, request->url);
    inkwell_str_copy(zip->member, sizeof zip->member, request->member);
    inkwell_str_copy(zip->staging, sizeof zip->staging, request->staging_dir);
    inkwell_str_copy(zip->stem, sizeof zip->stem, stem);
    zip->max_member_bytes = request->max_member_bytes;
    zip->step_timeout_ms = request->step_timeout_ms;
    zip->member_timeout_ms = request->member_timeout_ms;
    zip->on_done = request->on_done;
    zip->userdata = request->userdata;
    zip->state = INKWELL_ZIP_FETCH_MEASURING;

    /*
     * The HEAD first, because the range that follows it cannot be expressed without a length -
     * some CDNs refuse a suffix range outright.
     */
    struct inkwell_fetch_request head;
    memset(&head, 0, sizeof head);
    head.url = zip->url;
    head.method = INKWELL_FETCH_HEAD;
    head.timeout_ms = zip->step_timeout_ms;
    head.on_done = zip_on_fetch;
    head.userdata = zip;
    const int started = inkwell_fetch_start(fetch, &head, inkwell_time_monotonic_ms());
    if (started != 0) {
        zip->state = INKWELL_ZIP_FETCH_IDLE;
        zip->on_done = NULL;
        return started;
    }
    return 0;
}

void inkwell_zip_fetch_tick(struct inkwell_zip_fetch *zip, uint64_t now_ms) {
    (void)now_ms;
    if (zip == NULL || !zip->inflate_pending) {
        return;
    }
    zip->inflate_pending = false;
    zip_inflate(zip);
}

void inkwell_zip_fetch_cancel(struct inkwell_zip_fetch *zip) {
    if (zip == NULL) {
        return;
    }
    if (zip->fetch != NULL) {
        inkwell_fetch_cancel(zip->fetch);
    }
    zip->inflate_pending = false;
    zip_clean(zip);
    zip_remove(zip, ZIP_FILE_IMAGE);
    zip->on_done = NULL;
    zip->userdata = NULL;
    zip->state = INKWELL_ZIP_FETCH_IDLE;
}

bool inkwell_zip_fetch_busy(const struct inkwell_zip_fetch *zip) {
    if (zip == NULL) {
        return false;
    }
    return zip->state != INKWELL_ZIP_FETCH_IDLE &&
           zip->state != INKWELL_ZIP_FETCH_READY &&
           zip->state != INKWELL_ZIP_FETCH_FAILED;
}

unsigned inkwell_zip_fetch_progress(const struct inkwell_zip_fetch *zip) {
    if (zip == NULL) {
        return 0U;
    }
    /*
     * The member has landed by the time the inflate starts, so INFLATING is 100 and not 0.
     * Reported the other way the bar ran to full, dropped to empty and filled again - which is
     * what it did on the device the first time this was watched, and which reads as a zip
     * starting over rather than as a step finishing.
     */
    if (zip->state == INKWELL_ZIP_FETCH_READY ||
        zip->state == INKWELL_ZIP_FETCH_INFLATING) {
        return 100U;
    }
    if (zip->state != INKWELL_ZIP_FETCH_FETCHING ||
        zip->entry.compressed_size == 0U) {
        return 0U;
    }
    const uint64_t landed = zip_file_size(zip, ZIP_FILE_MEMBER);
    if (landed >= (uint64_t)zip->entry.compressed_size) {
        return 100U;
    }
    return (unsigned)((landed * 100U) / (uint64_t)zip->entry.compressed_size);
}

const char *inkwell_zip_fetch_output_path(const struct inkwell_zip_fetch *zip,
                                              char *out, size_t out_len) {
    if (zip == NULL || out == NULL || out_len == 0U ||
        zip->state != INKWELL_ZIP_FETCH_READY) {
        return NULL;
    }
    return zip_path(zip, ZIP_FILE_IMAGE, out, out_len) ? out : NULL;
}

uint64_t inkwell_zip_fetch_size(const struct inkwell_zip_fetch *zip) {
    return zip != NULL ? (uint64_t)zip->entry.uncompressed_size : 0U;
}
