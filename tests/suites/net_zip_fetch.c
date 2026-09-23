#define _POSIX_C_SOURCE 200809L

/*
 * One member out of a remote zip, end to end, against the bytes a real CDN serves.
 *
 * An HTTPS server on loopback (support/https_fixture.h) serves ranges out of two fixtures the
 * way a CDN serves them out of one 46 MB release archive: a 302 from the release host to the
 * CDN, a HEAD whose length is the file's rather than the redirect's, the tail window at
 * 46,194,237, the local header at 2,540,989 and the member's deflated bytes at 2,541,100. Every
 * one of those offsets is where that archive really keeps them - see tests/data/README.md.
 *
 * The member is 486 bytes deflated and 1,157 out, which is small enough to be a fixture and
 * exercises exactly the same steps as one a thousand times its size: range read, place the data,
 * inflate it, check the length and CRC the central directory carried.
 */

#include "framework/inkwell_test.h"

#include "inkwell/net/fetch.h"
#include "inkwell/net/zip_fetch.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef INKWELL_TEST_DATA_DIR
#define INKWELL_TEST_DATA_DIR "tests/data"
#endif

/* Larger than the fixture member and far smaller than the 2 GiB the oversizing CDN claims. */
#define TEST_MEMBER_LIMIT (32U * 1024U * 1024U)

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* The archive the two fixtures were cut from, and where they sit inside it. */
#define ZIP_SIZE "46259773"
#define TAIL_BASE 46194237
#define MEMBER_BASE 2540989

#define FIXTURE_MEMBER "firmware-heltec-mesh-node-t114-2.7.26.54e0d8d.mt.json"

struct zip_probe {
    unsigned calls;
    enum inkwell_zip_fetch_state state;
    enum inkwell_zip_fetch_error error;
    uint64_t size;
    char output[512];
};

static void zip_record(void *userdata, const struct inkwell_zip_fetch *zip) {
    struct zip_probe *const probe = (struct zip_probe *)userdata;
    probe->calls++;
    probe->state = zip->state;
    probe->error = zip->error;
    probe->size = inkwell_zip_fetch_size(zip);
    probe->output[0] = '\0';
    (void)inkwell_zip_fetch_output_path(zip, probe->output, sizeof probe->output);
}

/* What the fake CDN does to the bytes it serves. */
enum zip_cdn {
    CDN_HONEST,
    /* One byte of the member's payload zeroed: the member arrives and is wrong. */
    CDN_CORRUPT_MEMBER,
    /* That member's uncompressed size in the central directory rewritten to 2 GiB. */
    CDN_OVERSIZED_MEMBER,
};

/* Where that size field sits in the tail window: the member's central record starts at 50,000,
   and the uncompressed size is 24 bytes into a record. */
#define TAIL_MEMBER_SIZE_FIELD 50024

/* Reads `count` bytes at `offset` of a fixture into `out`. Returns how many it could. */
static size_t zip_slice(const char *name, uint64_t offset, uint64_t count, char *out) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", INKWELL_TEST_DATA_DIR, name);
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return 0U;
    }
    size_t got = 0U;
    if (fseek(file, (long)offset, SEEK_SET) == 0) {
        got = fread(out, 1U, (size_t)count, file);
    }
    fclose(file);
    return got;
}

/*
 * The fake CDN, in the fixture's child. `userdata` is the enum zip_cdn it behaves as.
 *
 * Every zip URL answers 302 to a second host, the way a release asset does - and the 302 carries
 * `content-length: 0`, so a HEAD that reported the first hop's length would measure every zip as
 * empty.
 */
static void zip_serve(void *userdata, const struct https_fixture_request *request,
                      struct https_fixture_conn *conn) {
    const enum zip_cdn cdn = *(const enum zip_cdn *)userdata;
    if (strcmp(request->host, "objects.githubusercontent.com") != 0) {
        https_fixture_reply(conn, 302, "Location: https://objects.githubusercontent.com/zip\r\n",
                            NULL, 0U);
        return;
    }
    if (strcmp(request->method, "HEAD") == 0) {
        https_fixture_printf(conn, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n"
                                   "Content-Length: " ZIP_SIZE "\r\n\r\n");
        return;
    }
    if (!request->ranged) {
        https_fixture_reply(conn, 403, NULL, NULL, 0U);
        return;
    }
    const uint64_t first = request->first;
    const uint64_t count = request->last - first + 1U;
    static char body[1024U * 1024U];
    size_t got = 0U;
    if (count <= sizeof body && first >= TAIL_BASE) {
        got = zip_slice("zip_tail_nrf52840_2.7.26.bin", first - TAIL_BASE, count, body);
        if (cdn == CDN_OVERSIZED_MEMBER && first == TAIL_BASE &&
            got >= TAIL_MEMBER_SIZE_FIELD + 4U) {
            memcpy(body + TAIL_MEMBER_SIZE_FIELD, "\x00\x00\x00\x80", 4U);
        }
    } else if (count <= sizeof body && first >= MEMBER_BASE) {
        got = zip_slice("zip_member_t114_mt_json_2.7.26.bin", first - MEMBER_BASE, count, body);
        if (cdn == CDN_CORRUPT_MEMBER && first - MEMBER_BASE == 111U && got > 200U) {
            body[200] = '\0';
        }
    } else {
        https_fixture_reply(conn, 416, NULL, NULL, 0U);
        return;
    }
    char range[96];
    snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/" ZIP_SIZE "\r\n",
             (unsigned long long)first, (unsigned long long)(first + got - 1U));
    https_fixture_reply(conn, 206, range, body, got);
}

/* Stands the fake CDN up as `cdn`, and points `fetch` at it. */
static bool zip_serve_as(struct https_fixture *server, enum zip_cdn cdn,
                         struct inkwell_fetch *fetch) {
    static enum zip_cdn mode;
    mode = cdn;
    https_fixture_stop(server);
    if (!https_fixture_start(server, zip_serve, &mode)) {
        return false;
    }
    https_fixture_attach(server, fetch);
    return true;
}

static bool zip_wait(struct inkwell_loop *loop, struct inkwell_fetch *fetch,
                     struct inkwell_zip_fetch *zip, const struct zip_probe *probe) {
    for (int turn = 0; turn < 600 && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(loop, 10);
        inkwell_fetch_tick(fetch, 0U);
        inkwell_zip_fetch_tick(zip, 0U);
    }
    return probe->calls > 0U;
}

static void zip_request(struct inkwell_zip_fetch_request *request, const char *member,
                        const char *dir, struct zip_probe *probe) {
    memset(request, 0, sizeof *request);
    request->url = "https://example.invalid/firmware-nrf52840-2.7.26.zip";
    request->member = member;
    request->staging_dir = dir;
    request->stem = "dl";
    request->max_member_bytes = TEST_MEMBER_LIMIT;
    request->step_timeout_ms = 20000U;
    request->member_timeout_ms = 20000U;
    request->on_done = zip_record;
    request->userdata = probe;
}

/* Removes the staged files and the temporary directory, whatever the case did. */
static void zip_clean_dir(const char *dir) {
    static const char *const k_files[] = {"dl.window", "dl.central", "dl.header", "dl.member",
                                          "dl.image"};
    char path[512];
    for (size_t i = 0; i < sizeof k_files / sizeof k_files[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, k_files[i]);
        (void)unlink(path);
    }
    (void)rmdir(dir);
}

static bool zip_exists(const char *dir, const char *name) {
    char path[512];
    struct stat info;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return stat(path, &info) == 0;
}

/*
 * One member out of a 46 MB zip in four range requests, verified by the inflate.
 *
 * The file on disk is the length the central directory promised, and its contents are the
 * document that archive really carries. The CRC and the length are checked on the way past, by
 * the inflate, which is why there is no separate verify step to test.
 */
INKWELL_TEST_CASE(zip_fetch_fetches_a_member_end_to_end, unit) {
    char dir[] = "/tmp/inkwell_zipfetch_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetch;
    struct inkwell_zip_fetch zip;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!zip_serve_as(&server, CDN_HONEST, &fetch)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }

    struct zip_probe probe;
    memset(&probe, 0, sizeof probe);
    memset(&zip, 0, sizeof zip);
    struct inkwell_zip_fetch_request request;
    zip_request(&request, FIXTURE_MEMBER, dir, &probe);
    if (inkwell_zip_fetch_start(&zip, &fetch, &request) != 0) {
        failure = "the download should start";
        goto cleanup;
    }
    if (!inkwell_zip_fetch_busy(&zip)) {
        failure = "and report itself busy while it runs";
        goto cleanup;
    }
    if (!zip_wait(&loop, &fetch, &zip, &probe)) {
        failure = "the download should have finished";
        goto cleanup;
    }
    if (probe.state != INKWELL_ZIP_FETCH_READY || probe.error != INKWELL_ZIP_FETCH_ERROR_NONE) {
        failure = "it should have finished ready, with no error";
        goto cleanup;
    }
    if (probe.size != 1157ULL) {
        failure = "the member is 1,157 bytes, as the central directory said";
        goto cleanup;
    }
    {
        const size_t len = strlen(probe.output);
        if (len < 9U || strcmp(probe.output + len - 9U, "/dl.image") != 0) {
            failure = "and it lands at <staging>/<stem>.image";
            goto cleanup;
        }
    }
    if (inkwell_zip_fetch_progress(&zip) != 100U) {
        failure = "a finished download reports 100";
        goto cleanup;
    }

    /* The far end of the chain: what landed is the document, not merely 1,157 bytes. */
    {
        FILE *const output = fopen(probe.output, "rb");
        if (output == NULL) {
            failure = "the staged member should be readable";
            goto cleanup;
        }
        char text[2048];
        const size_t got = fread(text, 1U, sizeof text - 1U, output);
        fclose(output);
        text[got] = '\0';
        if (got != 1157U || text[0] != '{' || strstr(text, "\"files\"") == NULL) {
            failure = "and it should be the JSON document the archive carries";
            goto cleanup;
        }
    }

    /* The intermediates are gone and the result is not: a retry should skip nothing it needs
       and keep nothing it does not. */
    if (zip_exists(dir, "dl.member") || zip_exists(dir, "dl.window") ||
        zip_exists(dir, "dl.header")) {
        failure = "the intermediates should have been cleaned up";
        goto cleanup;
    }
    if (!zip_exists(dir, "dl.image")) {
        failure = "and the result kept";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetch);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    zip_clean_dir(dir);
    INKWELL_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A member the archive does not carry, a member that did not survive the trip, and one whose
 * directory claims more than the caller will accept.
 *
 * Refusals that must not be one row. "There is no such file" is an answer about the archive; a
 * CRC that does not match is an answer about the network, and the second is worth retrying
 * where the first is not.
 */
INKWELL_TEST_CASE(zip_fetch_tells_a_missing_member_from_a_broken_one, unit) {
    char dir[] = "/tmp/inkwell_zipfetch_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(dir) == NULL, "could not create a temporary directory");

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    struct inkwell_fetch fetch;
    struct inkwell_zip_fetch zip;
    struct inkwell_zip_fetch_request request;
    struct zip_probe probe;
    bool loop_up = false;
    bool fetch_up = false;

    if (inkwell_loop_init(&loop) != 0) {
        failure = "event loop init failed";
        goto cleanup;
    }
    loop_up = true;
    if (inkwell_fetch_init(&fetch, &loop) != 0) {
        failure = "fetch init failed";
        goto cleanup;
    }
    fetch_up = true;
    if (!zip_serve_as(&server, CDN_HONEST, &fetch)) {
        failure = "could not stand up the fake CDN";
        goto cleanup;
    }

    /* A real file name this archive does not carry: the walk succeeds and finds nothing, which
       is not the same as the walk failing. */
    memset(&probe, 0, sizeof probe);
    memset(&zip, 0, sizeof zip);
    zip_request(&request, "firmware-heltec-v3-2.7.26.54e0d8d.bin", dir, &probe);
    if (inkwell_zip_fetch_start(&zip, &fetch, &request) != 0) {
        failure = "the download should start";
        goto cleanup;
    }
    if (!zip_wait(&loop, &fetch, &zip, &probe)) {
        failure = "it should have finished";
        goto cleanup;
    }
    if (probe.state != INKWELL_ZIP_FETCH_FAILED ||
        probe.error != INKWELL_ZIP_FETCH_ERROR_NO_MEMBER) {
        failure = "a file the archive does not carry is its own answer";
        goto cleanup;
    }

    /* Now the same fetch with one byte of the member's payload flipped. Everything up to the
       inflate succeeds, and the inflate is what catches it. */
    if (!zip_serve_as(&server, CDN_CORRUPT_MEMBER, &fetch)) {
        failure = "could not stand up the corrupting CDN";
        goto cleanup;
    }
    memset(&probe, 0, sizeof probe);
    memset(&zip, 0, sizeof zip);
    zip_request(&request, FIXTURE_MEMBER, dir, &probe);
    if (inkwell_zip_fetch_start(&zip, &fetch, &request) != 0) {
        failure = "the second download should start";
        goto cleanup;
    }
    if (!zip_wait(&loop, &fetch, &zip, &probe)) {
        failure = "it should have finished too";
        goto cleanup;
    }
    if (probe.state != INKWELL_ZIP_FETCH_FAILED || probe.error != INKWELL_ZIP_FETCH_ERROR_INFLATE) {
        failure = "a member that did not survive the trip is refused by the inflate";
        goto cleanup;
    }
    if (zip_exists(dir, "dl.image")) {
        failure = "and a failed download leaves no result behind for a retry to pick up";
        goto cleanup;
    }

    /* A directory claiming the member inflates to 2 GiB. The size is what gets allocated and
       inflated into on the loop, so it is refused before the member is even fetched. */
    if (!zip_serve_as(&server, CDN_OVERSIZED_MEMBER, &fetch)) {
        failure = "could not stand up the oversizing CDN";
        goto cleanup;
    }
    memset(&probe, 0, sizeof probe);
    memset(&zip, 0, sizeof zip);
    zip_request(&request, FIXTURE_MEMBER, dir, &probe);
    if (inkwell_zip_fetch_start(&zip, &fetch, &request) != 0) {
        failure = "the third download should start";
        goto cleanup;
    }
    if (!zip_wait(&loop, &fetch, &zip, &probe)) {
        failure = "it should have finished as well";
        goto cleanup;
    }
    if (probe.state != INKWELL_ZIP_FETCH_FAILED ||
        probe.error != INKWELL_ZIP_FETCH_ERROR_UNSUPPORTED) {
        failure = "a member bigger than the caller's limit is refused, not allocated";
        goto cleanup;
    }
    if (zip.located) {
        failure = "and refused off the directory, before its header or bytes were fetched";
        goto cleanup;
    }

cleanup:
    if (fetch_up) {
        inkwell_fetch_shutdown(&fetch);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    zip_clean_dir(dir);
    INKWELL_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */

/* The arguments that are refused before anything is started. */
INKWELL_TEST_CASE(zip_fetch_refuses_what_it_cannot_do, unit) {
    struct inkwell_fetch fetch;
    INKWELL_TEST_FAIL_IF(inkwell_fetch_init(&fetch, NULL) != 0, "a loopless fetcher should init");
    INKWELL_TEST_FAIL_IF(inkwell_fetch_available(&fetch), "and report itself unavailable");

    struct inkwell_zip_fetch zip;
    memset(&zip, 0, sizeof zip);
    struct inkwell_zip_fetch_request request;
    memset(&request, 0, sizeof request);
    request.url = "https://example.invalid/a.zip";
    request.member = "a";
    request.staging_dir = "/tmp";
    request.max_member_bytes = TEST_MEMBER_LIMIT;

    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, NULL) != -EINVAL,
                         "no request is -EINVAL");
    request.url = NULL;
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, &request) != -EINVAL,
                         "and so is no URL");
    request.url = "https://example.invalid/a.zip";
    request.member = "";
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, &request) != -EINVAL,
                         "and an empty member name");
    request.member = "a";
    request.max_member_bytes = 0U;
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, &request) != -EINVAL,
                         "and a caller that states no limit: there is no default to fall to");
    request.max_member_bytes = TEST_MEMBER_LIMIT;
    request.stem = "a-stem-that-is-much-too-long-to-be-a-prefix";
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, &request) != -EINVAL,
                         "and a stem that would be cut into a different name");
    request.stem = NULL;
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_start(&zip, &fetch, &request) != -ENOTSUP,
                         "with no fetcher there is nothing to try");
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_busy(&zip),
                         "and nothing was started, so nothing is running");
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_progress(&zip) != 0U,
                         "nor is there anything to report progress on");
    char path[64];
    INKWELL_TEST_FAIL_IF(inkwell_zip_fetch_output_path(&zip, path, sizeof path) != NULL,
                         "and no result to point at");
    inkwell_fetch_shutdown(&fetch);
    record_success(test_name);
}
