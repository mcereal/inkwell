#define _POSIX_C_SOURCE 200809L

/*
 * ECDSA P-256 verification: a signature made by another tool (OpenSSL) checks out, a changed
 * digest or signature does not, and a key off the curve is refused. Without Mbed TLS, every call
 * refuses.
 */

#include "framework/inkwell_test.h"

#include "inkwell/codec/sha256.h"
#include "inkwell/codec/signature.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void from_hex(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned byte = 0U;
        (void)sscanf(hex + 2U * i, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
}

/* `openssl dgst -sha256 -sign` over "abc" with a fresh prime256v1 key; DER turned into r || s. */
static const char k_key[] = "04ab1c37a4c81ae52106e867d7116a8bdf47b901334ecd974038f3fe85f427b97c"
                            "1b40d2579938ee7bcb773dcf107ed15e31c2565e5d57fb60d31be1e4a45e4ed2";
static const char k_signature[] =
    "f0898211bba2e541b404afd57f8dfb971aa8d4bb7fecf28a36962c7785e750b8"
    "a545c4a145a0eea9406067eaf9ca284a2706848511dc162876e41dd963987497";

INKWELL_TEST_CASE(signature_p256_verifies_what_openssl_signed, unit) {
    uint8_t key[INKWELL_P256_PUBLIC_KEY_LEN];
    uint8_t signature[INKWELL_P256_SIGNATURE_LEN];
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    from_hex(k_key, key, sizeof key);
    from_hex(k_signature, signature, sizeof signature);
    struct inkwell_sha256 ctx;
    inkwell_sha256_init(&ctx);
    inkwell_sha256_update(&ctx, "abc", 3U);
    inkwell_sha256_final(&ctx, digest);

    if (!inkwell_signature_available()) {
        INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != -ENOSYS,
                             "without Mbed TLS every check refuses");
        record_success(test_name);
        return;
    }
    INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != 0,
                         "a signature another tool made checks out");
    digest[0] ^= 1U;
    INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != -EBADMSG,
                         "over a different digest it does not");
    digest[0] ^= 1U;
    signature[40] ^= 0x80U;
    INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != -EBADMSG,
                         "nor a changed signature");
    signature[40] ^= 0x80U;
    key[64] ^= 1U;
    INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != -EINVAL,
                         "a key that is not a point on the curve is refused");
    key[64] ^= 1U;
    INKWELL_TEST_FAIL_IF(inkwell_signature_verify_p256(key, digest, signature) != 0,
                         "and the original still checks out after all that");
    record_success(test_name);
}
