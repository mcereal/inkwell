#pragma once

/*
 * Messages on a byte stream: each one a 4-byte little-endian length, then that many bytes.
 *
 * A stream (net/stream.h) delivers bytes in whatever pieces the kernel had - half a message, three
 * and a bit - and says outright that framing is somebody else's question. This is the answer for
 * a protocol whose messages are binary and bounded: a length prefix, because a sentinel would
 * have to be escaped inside a payload that can hold any byte.
 *
 * **The bound is the caller's, and it is enforced before a byte is buffered.** A frame whose
 * header announces more than the reader's buffer holds is refused the moment the header is
 * complete, and the reader refuses everything after it: once a length has been rejected there is
 * no knowing where the next frame starts, so the only sound thing left is for the owner to close
 * the stream. A peer cannot make the reader allocate, and cannot make it wait for four gigabytes.
 *
 * An empty frame is a frame. A protocol may give it a meaning (a keepalive); the reader delivers
 * it like any other.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INKWELL_FRAME_HEADER 4U

/*
 * Writes the header for a `len`-byte payload into `out`. The payload follows it, in the same
 * write or the next one - the stream keeps writes in order. Returns 0, or -EMSGSIZE when `len`
 * does not fit in 32 bits.
 */
int inkwell_frame_header(uint8_t out[INKWELL_FRAME_HEADER], size_t len);

/*
 * One whole frame. `payload` is only valid for the call. Returning non-zero stops the feed that
 * delivered it, and feed() returns that value - which is how a callback that has closed the
 * connection the reader belongs to stops the reader touching anything afterwards.
 */
typedef int (*inkwell_frame_fn)(void *userdata, const uint8_t *payload, size_t len);

struct inkwell_frame_reader {
    uint8_t *buffer; /* the caller's: one payload's worth */
    size_t capacity;
    uint8_t header[INKWELL_FRAME_HEADER];
    size_t header_have;
    size_t want; /* the payload length under way, once the header is whole */
    size_t have;
    bool broken; /* a length was refused: nothing after it can be framed */
};

/* An empty reader over `buffer`, the largest payload it will take being `capacity` bytes. */
void inkwell_frame_reader_init(struct inkwell_frame_reader *reader, uint8_t *buffer,
                               size_t capacity);

/*
 * Hands `len` arrived bytes to the reader, which calls `on_frame` for every frame they complete,
 * in order. Bytes of a frame not yet complete are kept for the next feed.
 *
 * Returns 0; the callback's non-zero return when it stopped the feed; or -EMSGSIZE when a header
 * announced more than the capacity, after which every feed returns -EMSGSIZE until the reader is
 * initialised again. The owner's answer to that is to close the stream.
 */
int inkwell_frame_reader_feed(struct inkwell_frame_reader *reader, const uint8_t *bytes, size_t len,
                              inkwell_frame_fn on_frame, void *userdata);

#ifdef __cplusplus
}
#endif
