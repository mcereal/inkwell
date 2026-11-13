#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 8018): turning a secret a person can
 * remember - a PIN, a passphrase - into one an attacker who has the stored result must guess
 * the slow way. Built on codec/sha256.h, so it is the same everywhere inkwell builds, with or
 * without Mbed TLS.
 *
 * PBKDF2 is the oldest of the password hashes and the plainest: it costs time but not memory, so
 * it is the weakest against a GPU. It is here because it is small and exact, and what stands
 * behind it on a device - an attempt limit with a growing wait - is what keeps a 4-digit PIN from
 * being tried 10 000 times. The count of iterations is the caller's: as many as the slowest
 * device it runs on can spend in a quarter second.
 */

#define INKWELL_HMAC_SHA256_LEN 32U

void inkwell_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len,
                         uint8_t out[INKWELL_HMAC_SHA256_LEN]);

/* `out_len` bytes derived from `password` and `salt` over `iterations` rounds. 0, or -EINVAL for
   no iterations or no output. */
int inkwell_pbkdf2_sha256(const void *password, size_t password_len, const void *salt,
                          size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);

/* Whether `a` and `b` agree over `len` bytes, taking the same time whatever their difference: the
   comparison a stored hash is checked with, so the time it took says nothing of how close a
   guess was. */
bool inkwell_equal_secret(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif
