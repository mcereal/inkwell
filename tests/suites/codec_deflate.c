/*
 * Writing: a zlib stream (codec/deflate.h) and a PNG (codec/png.h's encoder). Each is read back by
 * the decoder that is already here - inflate.h, and Wuffs' PNG - so what is held is that what is
 * written is what any reader gets, not the bytes of one particular encoding.
 */

#include "framework/inkwell_test.h"

#include "inkwell/codec/deflate.h"
#include "inkwell/codec/inflate.h"
#include "inkwell/codec/png.h"

#include <stdlib.h>
#include <string.h>

static uint32_t g_work[INKWELL_DEFLATE_WORK_BYTES / sizeof(uint32_t)];

/* `in` deflated and inflated again: whether it came back, and how long the stream was. */
static bool round_trip(const uint8_t *in, size_t len, size_t *stream_len) {
    const size_t cap = inkwell_deflate_bound(len);
    uint8_t *z = malloc(cap);
    uint8_t *back = malloc(len + 1U);
    bool ok = false;
    if (z != NULL && back != NULL) {
        const size_t n = inkwell_deflate_zlib(in, len, z, cap, g_work);
        *stream_len = n;
        size_t produced = 0U;
        /* zlib framing: two bytes before the deflate stream, the Adler-32 after. */
        ok = n >= 6U && z[0] == 0x78U && ((z[0] << 8) | z[1]) % 31 == 0 &&
             inkwell_inflate(z + 2U, n - 6U, back, len + 1U, &produced) == INKWELL_INFLATE_OK &&
             produced == len && (len == 0U || memcmp(back, in, len) == 0);
    }
    free(z);
    free(back);
    return ok;
}

INKWELL_TEST_CASE(deflate_round_trips_through_inflate, unit) {
    size_t n = 0U;
    INKWELL_TEST_FAIL_IF(!round_trip(NULL, 0U, &n), "nothing");
    static const char text[] =
        "the keyboard, the shade, the lock, the keyboard, the shade, the lock";
    INKWELL_TEST_FAIL_IF(!round_trip((const uint8_t *)text, sizeof text - 1U, &n), "words");
    INKWELL_TEST_FAIL_IF(n >= sizeof text, "repeated words come out shorter");

    /* A run far longer than one match, and longer than the window. */
    static uint8_t flat[200000];
    memset(flat, 0x2A, sizeof flat);
    INKWELL_TEST_FAIL_IF(!round_trip(flat, sizeof flat, &n), "a long run");
    INKWELL_TEST_FAIL_IF(n > sizeof flat / 100U, "a flat run deflates to almost nothing");

    /* Noise: no matches worth having, every byte a literal, and within the bound. */
    static uint8_t noise[70000];
    uint32_t seed = 12345U;
    for (size_t i = 0U; i < sizeof noise; ++i) {
        seed = seed * 1103515245U + 12345U;
        noise[i] = (uint8_t)(seed >> 16);
    }
    INKWELL_TEST_FAIL_IF(!round_trip(noise, sizeof noise, &n), "noise");

    uint8_t tiny[4];
    INKWELL_TEST_FAIL_IF(inkwell_deflate_zlib(noise, sizeof noise, tiny, sizeof tiny, g_work) != 0U,
                         "too small an out is refused, not overrun");
    record_success(test_name);
}

#define PIC_W 64U
#define PIC_H 48U

INKWELL_TEST_CASE(png_encode_writes_what_a_decoder_reads_back, unit) {
    /* A screen-like picture: a flat ground, a bar, a gradient. */
    static uint8_t pixels[PIC_W * PIC_H * 4U];
    for (uint32_t y = 0U; y < PIC_H; ++y) {
        for (uint32_t x = 0U; x < PIC_W; ++x) {
            uint8_t *p = &pixels[(y * PIC_W + x) * 4U];
            p[0] = y < 8U ? 0x30U : (uint8_t)(x * 4U); /* blue */
            p[1] = y < 8U ? 0x20U : (uint8_t)(y * 5U); /* green */
            p[2] = 0x10U;                              /* red */
            p[3] = 0x00U;                              /* unread */
        }
    }
    const size_t scratch_len = inkwell_png_encode_scratch_bytes(PIC_W, PIC_H);
    const size_t cap = inkwell_png_encode_bound(PIC_W, PIC_H);
    INKWELL_TEST_FAIL_IF(scratch_len == 0U || cap == 0U, "sizes");
    void *scratch = malloc(scratch_len);
    uint8_t *png = malloc(cap);
    INKWELL_TEST_FAIL_IF(scratch == NULL || png == NULL, "memory");
    const size_t len = inkwell_png_encode_bgrx(pixels, PIC_W, PIC_H, PIC_W * 4U, scratch, png, cap);
    INKWELL_TEST_FAIL_IF(len == 0U || len > cap, "a file");
    INKWELL_TEST_FAIL_IF(memcmp(png, "\x89PNG\r\n\x1a\n", 8U) != 0, "a PNG's signature");

    static struct inkwell_png_decoder decoder;
    static uint8_t back[PIC_W * PIC_H * 4U];
    const size_t work_len = inkwell_png_workbuf_bytes(PIC_W, PIC_H, 3U);
    uint8_t *work = malloc(work_len);
    const int decoded = inkwell_png_decode_bgra(&decoder, png, len, PIC_W, PIC_H, back, sizeof back,
                                                work, work_len);
    bool same = decoded == 0;
    for (size_t i = 0U; same && i < (size_t)PIC_W * PIC_H; ++i) {
        same = back[i * 4U] == pixels[i * 4U] && back[i * 4U + 1U] == pixels[i * 4U + 1U] &&
               back[i * 4U + 2U] == pixels[i * 4U + 2U] && back[i * 4U + 3U] == 0xFFU;
    }
    const bool narrow_refused =
        inkwell_png_encode_bgrx(pixels, PIC_W, PIC_H, PIC_W * 3U, scratch, png, cap) == 0U;
    free(work);
    free(scratch);
    free(png);
    INKWELL_TEST_FAIL_IF(!same, "every pixel back as it was, opaque");
    INKWELL_TEST_FAIL_IF(len >= (size_t)PIC_W * PIC_H * 3U / 4U,
                         "a picture of a screen comes out well under its raw size");
    INKWELL_TEST_FAIL_IF(!narrow_refused, "a stride narrower than a row is refused");
    record_success(test_name);
}
