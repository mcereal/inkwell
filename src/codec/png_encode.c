/*
 * Pixels into a PNG file (codec/png.h, inkwell_png_encode_bgrx()): the signature, IHDR, one IDAT
 * of the filtered rows deflated (codec/deflate.h), IEND. Each chunk's CRC is inflate.h's.
 */

#include "inkwell/codec/deflate.h"
#include "inkwell/codec/inflate.h"
#include "inkwell/codec/png.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* A filtered row: its filter byte, then three bytes a pixel. */
static size_t row_bytes(uint32_t width) {
    return 1U + (size_t)width * 3U;
}

size_t inkwell_png_encode_scratch_bytes(uint32_t width, uint32_t height) {
    if (width == 0U || height == 0U) {
        return 0U;
    }
#if SIZE_MAX <= UINT32_MAX
    /* Only a 32-bit size_t can overflow a row: a uint32_t width times three fits in 64 bits. */
    if ((size_t)width > (SIZE_MAX - 1U) / 3U / 2U) {
        return 0U;
    }
#endif
    const size_t row = row_bytes(width);
    if ((size_t)height > (SIZE_MAX - INKWELL_DEFLATE_WORK_BYTES) / row - 2U) {
        return 0U;
    }
    /* The work tables first, so they are aligned as the scratch is; then the filtered picture,
       then two rows for the previous row unfiltered and a candidate filter. */
    return INKWELL_DEFLATE_WORK_BYTES + row * ((size_t)height + 2U);
}

size_t inkwell_png_encode_bound(uint32_t width, uint32_t height) {
    const size_t scratch = inkwell_png_encode_scratch_bytes(width, height);
    if (scratch == 0U) {
        return 0U;
    }
    const size_t raw = row_bytes(width) * height;
    const size_t deflated = inkwell_deflate_bound(raw);
    return deflated == 0U ? 0U : deflated + 8U + 25U + 12U + 12U;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A chunk whose `len` bytes of data are already at `at + 8`: its length, type and CRC around it.
   Returns where the next chunk goes, or 0 when the CRC could not be made. */
static size_t chunk(uint8_t *out, size_t at, const char type[4], size_t len) {
    be32(&out[at], (uint32_t)len);
    memcpy(&out[at + 4U], type, 4U);
    uint32_t crc = 0U;
    if (!inkwell_crc32(&out[at + 4U], 4U + len, &crc)) {
        return 0U;
    }
    be32(&out[at + 8U + len], crc);
    return at + 12U + len;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = (int)a + (int)b - (int)c;
    const int pa = abs(p - (int)a);
    const int pb = abs(p - (int)b);
    const int pc = abs(p - (int)c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* Row `cur` (RGB) filtered by `filter` against `up` (the row above, or zeros) into `dst`; the sum
   of the bytes as signed, which is how small it is likely to deflate. */
static uint64_t filter_row(uint8_t filter, const uint8_t *cur, const uint8_t *up, size_t n,
                           uint8_t *dst) {
    uint64_t cost = 0U;
    dst[0] = filter;
    for (size_t i = 0U; i < n; ++i) {
        const uint8_t a = i >= 3U ? cur[i - 3U] : 0U;
        const uint8_t b = up[i];
        const uint8_t c = i >= 3U ? up[i - 3U] : 0U;
        uint8_t v = cur[i];
        switch (filter) {
        case 1:
            v = (uint8_t)(v - a);
            break;
        case 2:
            v = (uint8_t)(v - b);
            break;
        case 3:
            v = (uint8_t)(v - (uint8_t)(((unsigned)a + b) / 2U));
            break;
        case 4:
            v = (uint8_t)(v - paeth(a, b, c));
            break;
        default:
            break;
        }
        dst[1U + i] = v;
        cost += v < 128U ? v : 256U - v;
    }
    return cost;
}

size_t inkwell_png_encode_bgrx(const uint8_t *pixels, uint32_t width, uint32_t height,
                               size_t stride, void *scratch, uint8_t *out, size_t cap) {
    const size_t need = inkwell_png_encode_scratch_bytes(width, height);
    if (pixels == NULL || scratch == NULL || out == NULL || need == 0U ||
        stride < (size_t)width * 4U || cap < 8U + 25U + 12U + 12U) {
        return 0U;
    }
    uint32_t *work = scratch;
    uint8_t *raw = (uint8_t *)scratch + INKWELL_DEFLATE_WORK_BYTES;
    const size_t row = row_bytes(width);
    const size_t n = row - 1U;
    uint8_t *up = raw + row * height; /* the previous row, RGB, unfiltered */
    uint8_t *cur = up + row;          /* this one */
    memset(up, 0, n);
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *src = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            cur[x * 3U] = src[x * 4U + 2U];
            cur[x * 3U + 1U] = src[x * 4U + 1U];
            cur[x * 3U + 2U] = src[x * 4U];
        }
        /* Each filter tried into the row's place; the smallest kept. */
        uint8_t *dst = raw + row * y;
        uint8_t best = 0U;
        uint64_t best_cost = UINT64_MAX;
        for (uint8_t f = 0U; f <= 4U; ++f) {
            const uint64_t cost = filter_row(f, cur, up, n, dst);
            if (cost < best_cost) {
                best_cost = cost;
                best = f;
            }
        }
        if (best != 4U) {
            (void)filter_row(best, cur, up, n, dst);
        }
        memcpy(up, cur, n);
    }

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(out, signature, sizeof signature);
    size_t at = sizeof signature;
    uint8_t *ihdr = &out[at + 8U];
    be32(&ihdr[0], width);
    be32(&ihdr[4], height);
    ihdr[8] = 8U;  /* bits a channel */
    ihdr[9] = 2U;  /* RGB */
    ihdr[10] = 0U; /* deflate */
    ihdr[11] = 0U; /* the five filters */
    ihdr[12] = 0U; /* not interlaced */
    at = chunk(out, at, "IHDR", 13U);
    if (at == 0U || cap - at < 12U + 12U) {
        return 0U;
    }
    const size_t deflated =
        inkwell_deflate_zlib(raw, row * height, &out[at + 8U], cap - at - 12U - 12U, work);
    if (deflated == 0U) {
        return 0U;
    }
    at = chunk(out, at, "IDAT", deflated);
    if (at == 0U) {
        return 0U;
    }
    at = chunk(out, at, "IEND", 0U);
    return at;
}
