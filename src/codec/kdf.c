#include "inkwell/codec/kdf.h"

#include "inkwell/base/wipe.h"
#include "inkwell/codec/sha256.h"

#include <errno.h>
#include <string.h>

#define BLOCK 64U

void inkwell_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len,
                         uint8_t out[INKWELL_HMAC_SHA256_LEN]) {
    uint8_t k[BLOCK];
    memset(k, 0, sizeof k);
    struct inkwell_sha256 h;
    if (key_len > BLOCK) {
        /* A key longer than a block is its digest (RFC 2104 section 2). */
        inkwell_sha256_init(&h);
        inkwell_sha256_update(&h, key, key_len);
        inkwell_sha256_final(&h, k);
    } else if (key_len > 0U) {
        memcpy(k, key, key_len);
    }
    uint8_t pad[BLOCK];
    for (size_t i = 0U; i < BLOCK; ++i) {
        pad[i] = (uint8_t)(k[i] ^ 0x36U);
    }
    uint8_t inner[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_init(&h);
    inkwell_sha256_update(&h, pad, BLOCK);
    inkwell_sha256_update(&h, msg, msg_len);
    inkwell_sha256_final(&h, inner);
    for (size_t i = 0U; i < BLOCK; ++i) {
        pad[i] = (uint8_t)(k[i] ^ 0x5CU);
    }
    inkwell_sha256_init(&h);
    inkwell_sha256_update(&h, pad, BLOCK);
    inkwell_sha256_update(&h, inner, sizeof inner);
    inkwell_sha256_final(&h, out);
    inkwell_wipe(k, sizeof k);
    inkwell_wipe(pad, sizeof pad);
    inkwell_wipe(inner, sizeof inner);
    inkwell_wipe(&h, sizeof h);
}

int inkwell_pbkdf2_sha256(const void *password, size_t password_len, const void *salt,
                          size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len) {
    if (iterations == 0U || out == NULL || out_len == 0U || salt_len > 1024U) {
        return -EINVAL;
    }
    uint8_t first[1024U + 4U];
    memcpy(first, salt, salt_len);
    uint8_t u[INKWELL_HMAC_SHA256_LEN];
    uint8_t t[INKWELL_HMAC_SHA256_LEN];
    for (uint32_t block = 1U; out_len > 0U; ++block) {
        /* T_i = U_1 ^ ... ^ U_c, U_1 = HMAC(P, S || INT(i)), U_j = HMAC(P, U_{j-1}). */
        first[salt_len] = (uint8_t)(block >> 24);
        first[salt_len + 1U] = (uint8_t)(block >> 16);
        first[salt_len + 2U] = (uint8_t)(block >> 8);
        first[salt_len + 3U] = (uint8_t)block;
        inkwell_hmac_sha256(password, password_len, first, salt_len + 4U, u);
        memcpy(t, u, sizeof t);
        for (uint32_t j = 1U; j < iterations; ++j) {
            inkwell_hmac_sha256(password, password_len, u, sizeof u, u);
            for (size_t k = 0U; k < sizeof t; ++k) {
                t[k] ^= u[k];
            }
        }
        const size_t n = out_len < sizeof t ? out_len : sizeof t;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    inkwell_wipe(u, sizeof u);
    inkwell_wipe(t, sizeof t);
    inkwell_wipe(first, sizeof first);
    return 0;
}

bool inkwell_equal_secret(const void *a, const void *b, size_t len) {
    const volatile uint8_t *x = a;
    const volatile uint8_t *y = b;
    uint8_t diff = 0U;
    for (size_t i = 0U; i < len; ++i) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0U;
}
