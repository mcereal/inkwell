#include "inkwell/codec/signature.h"

#include <errno.h>

/* No Mbed TLS in this build: the arithmetic is not here, so every check refuses. */

bool inkwell_signature_available(void) {
    return false;
}

int inkwell_signature_verify_p256(const uint8_t key[INKWELL_P256_PUBLIC_KEY_LEN],
                                  const uint8_t digest[INKWELL_SHA256_DIGEST_LEN],
                                  const uint8_t signature[INKWELL_P256_SIGNATURE_LEN]) {
    (void)key;
    (void)digest;
    (void)signature;
    return -ENOSYS;
}
