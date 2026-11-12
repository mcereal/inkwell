#define _POSIX_C_SOURCE 200809L

/* Length-prefixed frames out of bytes that arrive in any pieces. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/frame.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

struct frames {
    size_t count;
    size_t len[8];
    uint8_t first[8];
    int stop_after; /* 0: never */
};

static int collect(void *userdata, const uint8_t *payload, size_t len) {
    struct frames *f = userdata;
    if (f->count < 8U) {
        f->len[f->count] = len;
        f->first[f->count] = len > 0U ? payload[0] : 0U;
    }
    f->count++;
    return f->stop_after != 0 && (int)f->count == f->stop_after ? 42 : 0;
}

/* Two frames - "abc" and the empty one - then "de", one stream of bytes. */
static size_t three_frames(uint8_t *out) {
    size_t at = 0U;
    (void)inkwell_frame_header(out + at, 3U);
    memcpy(out + at + 4U, "abc", 3U);
    at += 7U;
    (void)inkwell_frame_header(out + at, 0U);
    at += 4U;
    (void)inkwell_frame_header(out + at, 2U);
    memcpy(out + at + 4U, "de", 2U);
    return at + 6U;
}

INKWELL_TEST_CASE(frame_reads_the_same_in_any_pieces, unit) {
    uint8_t stream[32];
    const size_t total = three_frames(stream);
    INKWELL_TEST_FAIL_IF(stream[0] != 3U || stream[1] != 0U, "the header is little-endian");

    /* All at once, then one byte at a time: the same three frames. */
    for (size_t piece = total; piece >= 1U; piece = piece == total ? 1U : 0U) {
        uint8_t buffer[8];
        struct inkwell_frame_reader reader;
        inkwell_frame_reader_init(&reader, buffer, sizeof buffer);
        struct frames f = {0};
        for (size_t at = 0U; at < total; at += piece) {
            const size_t n = total - at < piece ? total - at : piece;
            INKWELL_TEST_FAIL_IF(inkwell_frame_reader_feed(&reader, stream + at, n, collect, &f) !=
                                     0,
                                 "a feed of whole or partial frames succeeds");
        }
        INKWELL_TEST_FAIL_IF(f.count != 3U || f.len[0] != 3U || f.first[0] != 'a' ||
                                 f.len[1] != 0U || f.len[2] != 2U || f.first[2] != 'd',
                             "abc, the empty frame and de, whatever the pieces");
    }
    record_success(test_name);
}

INKWELL_TEST_CASE(frame_refuses_a_length_past_the_buffer_for_good, unit) {
    uint8_t buffer[4];
    struct inkwell_frame_reader reader;
    inkwell_frame_reader_init(&reader, buffer, sizeof buffer);
    struct frames f = {0};
    uint8_t header[INKWELL_FRAME_HEADER];
    (void)inkwell_frame_header(header, 5U);
    INKWELL_TEST_FAIL_IF(inkwell_frame_reader_feed(&reader, header, sizeof header, collect, &f) !=
                             -EMSGSIZE,
                         "a frame larger than the buffer is refused at its header");
    (void)inkwell_frame_header(header, 1U);
    INKWELL_TEST_FAIL_IF(inkwell_frame_reader_feed(&reader, header, sizeof header, collect, &f) !=
                             -EMSGSIZE,
                         "and nothing after it is framed: there is no knowing where it starts");
    INKWELL_TEST_FAIL_IF(f.count != 0U, "nothing was delivered");
    record_success(test_name);
}

INKWELL_TEST_CASE(frame_a_callback_can_stop_the_feed, unit) {
    uint8_t stream[32];
    const size_t total = three_frames(stream);
    uint8_t buffer[8];
    struct inkwell_frame_reader reader;
    inkwell_frame_reader_init(&reader, buffer, sizeof buffer);
    struct frames f = {.stop_after = 1};
    INKWELL_TEST_FAIL_IF(inkwell_frame_reader_feed(&reader, stream, total, collect, &f) != 42,
                         "the callback's stop is what feed returns");
    INKWELL_TEST_FAIL_IF(f.count != 1U, "and no frame after it is delivered");
    record_success(test_name);
}
