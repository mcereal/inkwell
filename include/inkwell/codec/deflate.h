#pragma once

/*
 * Bytes into a zlib stream (RFC 1950 around RFC 1951 deflate): the other half of inflate.h, for a
 * PNG an application writes (png.h, inkwell_png_encode_bgrx()) and anything else that needs one.
 *
 * Small and predictable rather than as tight as zlib. One block, the fixed Huffman codes, and
 * LZ77 over the whole 32 KiB window with hash chains walked a bounded distance and the longest
 * match taken greedily. What it is for is pictures of user interfaces - flat colour, repeated
 * rows - where the matches are long and the codes hardly matter, and there it lands within a few
 * per cent of zlib's default. A photograph comes out larger than zlib would make it, and still
 * well formed.
 *
 * No allocation, as png.h: the hash chains live in `work`, INKWELL_DEFLATE_WORK_BYTES of it, which
 * the caller provides and which holds nothing between calls.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The scratch a call needs: two tables of 32 Ki positions. */
#define INKWELL_DEFLATE_WORK_BYTES (2U * 32768U * sizeof(uint32_t))

/*
 * The most a stream of `len` bytes can come to: every byte a nine-bit literal, plus the framing.
 * 0 for a length that would overflow.
 */
size_t inkwell_deflate_bound(size_t len);

/*
 * `in` as a zlib stream into `out`, which holds `cap` bytes; `work` is INKWELL_DEFLATE_WORK_BYTES
 * of scratch, any alignment that suits a uint32_t. Returns the stream's length, or 0 when `out`
 * is too small (a `cap` of inkwell_deflate_bound(len) never is) or an argument is not usable.
 */
size_t inkwell_deflate_zlib(const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                            uint32_t *work);

#ifdef __cplusplus
}
#endif
