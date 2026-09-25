#pragma once

/*
 * One file out of a zip on an HTTPS server, without downloading the zip.
 *
 * A release archive is often a hundred times the size of the one member a caller wants: 0.6 MB
 * of a 58 MB zip is the case this was written for, and the ratio gets better as archives grow
 * rather than worse. A zip keeps its table of contents at the end, so the member can be found
 * and fetched with range requests alone - four of them, in the order a zip has to be read:
 *
 *   1. HEAD            how long is the file. Not decoration: some CDNs answer
 *                      `501 Unsupported client range` to a suffix range, so the tail window has
 *                      to be asked for as an explicit `bytes=<start>-<end>`.
 *   2. the tail        64 KB, holding the end-of-central-directory record and - for every
 *                      archive measured - the whole central directory with it. When it does not,
 *                      the directory is fetched on its own and this is five steps.
 *   3. a local header  30 bytes, for the two field lengths that place the data.
 *   4. the member      deflated or stored, `compressed_size` bytes.
 *
 * Then the inflate, and the inflate is also where the download's integrity check comes from.
 * The member is inflated in memory (codec/inflate.h), and what comes out must be the length and
 * the CRC32 the central directory promised. There is no separate verify step: a truncated
 * member, a flipped byte and a wrong size all fail there, as ERROR_INFLATE.
 *
 * It ends with the member on disk, checked, and nothing done with it. What the member *is*, and
 * whether it is the right one, is the caller's question - this layer knows it has a name and a
 * CRC, not what it is for.
 *
 * Two numbers are the caller's rather than this layer's, for the reason every policy is: the
 * largest member it will accept, and how long each step may take. The first is not optional.
 * Both sizes come from a directory somebody else served, and the uncompressed one is what is
 * allocated and inflated into on the loop - an unbounded claim is a malloc that overcommit lets
 * succeed and an inflate that stalls the process until the kernel kills it. Only the caller
 * knows how big a real member can be, so it has to say.
 */

#include "inkwell/codec/zip.h"
#include "inkwell/net/fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* The URL is copied, because every step re-issues it. */
#define INKWELL_ZIP_FETCH_URL_MAX 512U
/* The prefix the staged files are named with: "<stem>.window", "<stem>.member", ... */
#define INKWELL_ZIP_FETCH_STEM_MAX 32U
/*
 * The largest central directory fetched on its own, when it does not fit the tail window. Its
 * size is a field the server wrote and it is read whole into memory, so it is bounded here. 16
 * MiB is a hundred thousand entries with long names - far past any archive this is for, and far
 * short of the four gigabytes the field can say.
 */
#define INKWELL_ZIP_FETCH_DIRECTORY_MAX (16U * 1024U * 1024U)

/* Where the download is. Each is a different thing for a caller to say, which is why they are
   states rather than a boolean and a percentage. */
enum inkwell_zip_fetch_state {
    INKWELL_ZIP_FETCH_IDLE = 0,
    INKWELL_ZIP_FETCH_MEASURING, /* HEAD: how long is the zip */
    INKWELL_ZIP_FETCH_READING,   /* the tail window, and the directory if it did not fit */
    INKWELL_ZIP_FETCH_LOCATING,  /* the local header */
    INKWELL_ZIP_FETCH_FETCHING,  /* the member - the only step with a bar worth drawing */
    INKWELL_ZIP_FETCH_INFLATING,
    INKWELL_ZIP_FETCH_READY,
    INKWELL_ZIP_FETCH_FAILED,
    INKWELL_ZIP_FETCH_STATE_COUNT,
};

/* Why it failed. Told apart because two of them mean "try again" and the rest do not. */
enum inkwell_zip_fetch_error {
    INKWELL_ZIP_FETCH_ERROR_NONE = 0,
    /* A step's fetch failed or timed out, or a range came back longer or shorter than it was
       asked for. Retryable. */
    INKWELL_ZIP_FETCH_ERROR_NETWORK,
    /* The staging directory could not be written to, or a staged file could not be read back. */
    INKWELL_ZIP_FETCH_ERROR_STAGING,
    /*
     * What came back is not a zip: no end-of-central-directory record, or a directory that
     * stopped being one partway through the walk. Also what a captive portal's login page looks
     * like from here. Distinct from NO_MEMBER on purpose - one is a fact about the archive and
     * the other a fact about what it contains, and only one of them is worth retrying.
     */
    INKWELL_ZIP_FETCH_ERROR_NOT_A_ZIP,
    /* The zip is fine and does not contain that file - a real answer, or a member that is
       empty, which the caller did not ask for either. */
    INKWELL_ZIP_FETCH_ERROR_NO_MEMBER,
    /* Compressed with something that is neither deflate nor store, or claiming to be bigger
       than the caller's limit - which is refused before a byte of it is fetched - or a
       directory bigger than INKWELL_ZIP_FETCH_DIRECTORY_MAX. */
    INKWELL_ZIP_FETCH_ERROR_UNSUPPORTED,
    /* Not a deflate stream, or not the length and CRC the directory described. The bytes
       arrived and they are not the bytes that were promised. Retryable, once. */
    INKWELL_ZIP_FETCH_ERROR_INFLATE,
    INKWELL_ZIP_FETCH_ERROR_COUNT,
};

/* Called once per started download, from the loop, when it has finished or failed. */
struct inkwell_zip_fetch;
typedef void (*inkwell_zip_fetch_done_fn)(void *userdata, const struct inkwell_zip_fetch *zip);

struct inkwell_zip_fetch_request {
    /* https, as inkwell_fetch requires. Redirects are followed on every step. */
    const char *url;
    /* A **basename**, matched as codec/zip.h matches one: the path in front of a member tends
       to move between versions of an archive, and its name does not. */
    const char *member;
    /* The directory the staged files are written into. It must exist. */
    const char *staging_dir;
    /* What they are named: "<stem>.window" and so on, and "<stem>.image" for the result. NULL
       or "" is "zip_fetch". A download is one at a time, so the names are fixed rather than
       unique, and a leftover from a run that died is overwritten rather than tripped over. */
    const char *stem;
    /* The most the member may claim to be, compressed or not. Required: see the top of this
       file for why there is no default. */
    uint32_t max_member_bytes;
    /* Each of the three small steps gets this long; 0 is the fetcher's own default. */
    uint32_t step_timeout_ms;
    /* The member's step, which is the one that carries the bytes; 0 is the fetcher's default. */
    uint32_t member_timeout_ms;
    /* How long any range read may go without a byte once connected (inkwell_fetch_request's
       idle_timeout_ms); 0 for no limit beyond the step's own. */
    uint32_t idle_timeout_ms;
    inkwell_zip_fetch_done_fn on_done;
    void *userdata;
};

struct inkwell_zip_fetch {
    /* Borrowed. The caller owns the fetcher and may not use it while a download is running:
       one request at a time is the fetcher's rule, not this module's. */
    struct inkwell_fetch *fetch;
    enum inkwell_zip_fetch_state state;
    enum inkwell_zip_fetch_error error;
    char url[INKWELL_ZIP_FETCH_URL_MAX];
    char member[INKWELL_ZIP_NAME_MAX];
    char staging[INKWELL_FETCH_PATH_MAX];
    char stem[INKWELL_ZIP_FETCH_STEM_MAX];
    uint32_t max_member_bytes;
    uint32_t step_timeout_ms;
    uint32_t member_timeout_ms;
    uint32_t idle_timeout_ms;
    /* The range read in flight, and whether it has already been asked for twice: a read that
       failed on the way (NETWORK or TIMED_OUT) is sent once more before the download fails. */
    void (*step)(struct inkwell_zip_fetch *zip);
    bool retried;

    /* Filled in as the steps answer. */
    uint64_t zip_size;
    uint64_t central_offset;
    uint64_t window_offset;
    size_t window_len;
    /* Set when the tail window did not hold the whole central directory and it was fetched on
       its own - which decides both what the next read lands in and how it is walked. */
    bool directory_only;
    /* The end record's entry count, carried to the walk of a directory fetched on its own. */
    uint32_t central_entries;
    struct inkwell_zip_entry entry;
    /* The directory has been read and the member passed every check on it; set before its
       header or its bytes are asked for. */
    bool located;
    uint64_t data_offset;
    /* The member has landed and the next inkwell_zip_fetch_tick() inflates it. */
    bool inflate_pending;

    /* Stable for the duration of a request, because the fetcher holds pointers to them. */
    char active_path[INKWELL_FETCH_PATH_MAX];
    char range[64];

    inkwell_zip_fetch_done_fn on_done;
    void *userdata;
};

/*
 * Puts `zip` in the idle state. Required before the first start() unless the struct is already
 * zeroed - static storage, `= {0}`, or a memset of a struct that holds it - because start()
 * asks whether a download is already running, and an automatic struct left uninitialised has
 * no answer to that.
 */
void inkwell_zip_fetch_init(struct inkwell_zip_fetch *zip);

/*
 * Starts fetching `request->member` out of the zip at `request->url`. The request is copied;
 * nothing in it need outlive the call.
 *
 * Returns 0, or -errno: -EINVAL for a missing argument or a zero `max_member_bytes`, or a name
 * that does not fit; -EBUSY when this or the fetcher is already running; -ENOTSUP when the
 * fetcher is unavailable. On any error nothing was started and `on_done` will not be called; on
 * 0 it is called exactly once, later, from the loop.
 */
int inkwell_zip_fetch_start(struct inkwell_zip_fetch *zip, struct inkwell_fetch *fetch,
                            const struct inkwell_zip_fetch_request *request);

/*
 * Runs the inflate once the member has landed, and so is where a download that got that far
 * finishes. Call every loop turn, beside inkwell_fetch_tick(): the inflate is a burst of work on
 * the loop's thread, and doing it from the tick rather than from inside the fetcher's completion
 * keeps the fetcher's callback short.
 */
void inkwell_zip_fetch_tick(struct inkwell_zip_fetch *zip, uint64_t now_ms);

/* Stops anything in flight, removes every staged file including the result, and does not report
   an outcome. For a caller that has decided the answer itself - a cancelled press, a shutdown. */
void inkwell_zip_fetch_cancel(struct inkwell_zip_fetch *zip);

/* True while a download is running. */
bool inkwell_zip_fetch_busy(const struct inkwell_zip_fetch *zip);

/*
 * How far the member has landed, 0-100, or 0 before the fetch of it begins.
 *
 * Measured against the **compressed** size from the central directory, because what is landing
 * is the zip member; dividing by the uncompressed size would stop the bar at a third and call it
 * done. INFLATING is 100, not 0: reported the other way the bar runs to full, drops to empty and
 * fills again, which reads as a download starting over rather than as a step finishing.
 */
unsigned inkwell_zip_fetch_progress(const struct inkwell_zip_fetch *zip);

/*
 * Where the finished member is, or NULL until it is. Valid while the download is not restarted;
 * the file is the caller's to use and to delete.
 */
const char *inkwell_zip_fetch_output_path(const struct inkwell_zip_fetch *zip, char *out,
                                          size_t out_len);

/* What the member inflates to, from the central directory - 0 before the directory is read. */
uint64_t inkwell_zip_fetch_size(const struct inkwell_zip_fetch *zip);

#ifdef __cplusplus
}
#endif
