#include "inkwell/codec/md5.h"

#include <string.h>

/* RFC 1321. The per-round shifts and the sine table, as the RFC gives them. */
static const uint8_t k_shift[64] = {
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U,
};

static const uint32_t k_sine[64] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
    0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
    0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
    0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
    0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
    0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
    0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
    0xeb86d391U,
};

static uint32_t rotl(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32U - bits));
}

static void md5_block(struct inkwell_md5 *ctx, const uint8_t block[64]) {
    uint32_t m[16];
    for (unsigned i = 0; i < 16U; ++i) {
        m[i] = (uint32_t)block[4U * i] | ((uint32_t)block[4U * i + 1U] << 8) |
               ((uint32_t)block[4U * i + 2U] << 16) | ((uint32_t)block[4U * i + 3U] << 24);
    }
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    for (unsigned i = 0; i < 64U; ++i) {
        uint32_t f;
        unsigned g;
        if (i < 16U) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32U) {
            f = (d & b) | (~d & c);
            g = (5U * i + 1U) % 16U;
        } else if (i < 48U) {
            f = b ^ c ^ d;
            g = (3U * i + 5U) % 16U;
        } else {
            f = c ^ (b | ~d);
            g = (7U * i) % 16U;
        }
        const uint32_t next = d;
        d = c;
        c = b;
        b = b + rotl(a + f + k_sine[i] + m[g], k_shift[i]);
        a = next;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void inkwell_md5_init(struct inkwell_md5 *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->bytes = 0U;
    ctx->block_len = 0U;
}

void inkwell_md5_update(struct inkwell_md5 *ctx, const void *data, size_t len) {
    const uint8_t *bytes = data;
    ctx->bytes += len;
    while (len > 0U) {
        const size_t room = sizeof ctx->block - ctx->block_len;
        const size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, bytes, take);
        ctx->block_len += take;
        bytes += take;
        len -= take;
        if (ctx->block_len == sizeof ctx->block) {
            md5_block(ctx, ctx->block);
            ctx->block_len = 0U;
        }
    }
}

void inkwell_md5_final(struct inkwell_md5 *ctx, uint8_t out[INKWELL_MD5_DIGEST_LEN]) {
    const uint64_t bits = ctx->bytes * 8U;
    static const uint8_t k_pad = 0x80U;
    static const uint8_t k_zero = 0U;
    /* The length is read before the padding goes through update(), which counts it too. */
    inkwell_md5_update(ctx, &k_pad, 1U);
    while (ctx->block_len != 56U) {
        inkwell_md5_update(ctx, &k_zero, 1U);
    }
    uint8_t length[8];
    for (unsigned i = 0; i < 8U; ++i) {
        length[i] = (uint8_t)(bits >> (8U * i));
    }
    inkwell_md5_update(ctx, length, sizeof length);
    for (unsigned i = 0; i < 4U; ++i) {
        out[4U * i] = (uint8_t)ctx->state[i];
        out[4U * i + 1U] = (uint8_t)(ctx->state[i] >> 8);
        out[4U * i + 2U] = (uint8_t)(ctx->state[i] >> 16);
        out[4U * i + 3U] = (uint8_t)(ctx->state[i] >> 24);
    }
}

void inkwell_md5_hex(const uint8_t digest[INKWELL_MD5_DIGEST_LEN], char *out, size_t out_len) {
    static const char k_hex[] = "0123456789abcdef";
    if (out == NULL || out_len < INKWELL_MD5_HEX_LEN) {
        if (out != NULL && out_len > 0U) {
            out[0] = '\0';
        }
        return;
    }
    for (size_t i = 0; i < INKWELL_MD5_DIGEST_LEN; ++i) {
        out[2U * i] = k_hex[digest[i] >> 4];
        out[2U * i + 1U] = k_hex[digest[i] & 0x0FU];
    }
    out[2U * INKWELL_MD5_DIGEST_LEN] = '\0';
}
