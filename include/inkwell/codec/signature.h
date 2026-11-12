#pragma once

/*
 * A signature checked against a public key: ECDSA over NIST P-256, of a SHA-256 digest
 * (codec/sha256.h). Verification only - a device checks what it is handed and never holds a
 * private key, so signing is the producer's tool's business, not this layer's.
 *
 * The key is the uncompressed point, 0x04 || X || Y; the signature the raw r || s, each 32 bytes
 * big-endian - the forms PSA and every ECDSA tool can produce, with no ASN.1 to parse on the
 * device.
 *
 * Mbed TLS does the arithmetic, so a build without it (net/tls.h's arrangement) has this header
 * and refuses every call with -ENOSYS: inkwell_signature_available() says which.
 */

#include <stdbool.h>
#include <stdint.h>

#include "inkwell/codec/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INKWELL_P256_PUBLIC_KEY_LEN 65U
#define INKWELL_P256_SIGNATURE_LEN 64U

bool inkwell_signature_available(void);

/*
 * Whether `signature` is `key`'s over `digest`. 0 when it is; -EBADMSG when it is not;
 * -EINVAL when `key` is not a point on the curve; -ENOSYS without Mbed TLS; -EIO when the crypto
 * library will not start.
 */
int inkwell_signature_verify_p256(const uint8_t key[INKWELL_P256_PUBLIC_KEY_LEN],
                                  const uint8_t digest[INKWELL_SHA256_DIGEST_LEN],
                                  const uint8_t signature[INKWELL_P256_SIGNATURE_LEN]);

#ifdef __cplusplus
}
#endif
