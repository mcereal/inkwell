#pragma once

/*
 * MD5, for checking what a peer says it holds against what was sent to it.
 *
 * Not for anything that has to resist an adversary - MD5 has not been that for twenty years. It
 * is here because some bootloaders answer "what is in this region" with an MD5 and nothing
 * else, and the only way to hear that answer is to compute the same digest on this side.
 * Anything choosing a digest for itself should take SHA-256 (`codec/sha256.h`).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INKWELL_MD5_DIGEST_LEN 16U
/* 32 hex digits and a NUL. */
#define INKWELL_MD5_HEX_LEN 33U

struct inkwell_md5 {
    uint32_t state[4];
    uint64_t bytes;
    uint8_t block[64];
    size_t block_len;
};

void inkwell_md5_init(struct inkwell_md5 *ctx);
void inkwell_md5_update(struct inkwell_md5 *ctx, const void *data, size_t len);
void inkwell_md5_final(struct inkwell_md5 *ctx, uint8_t out[INKWELL_MD5_DIGEST_LEN]);

/* Lower-case hex, NUL-terminated. `out_len` must be at least INKWELL_MD5_HEX_LEN. */
void inkwell_md5_hex(const uint8_t digest[INKWELL_MD5_DIGEST_LEN], char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
