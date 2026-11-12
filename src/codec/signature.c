#include "inkwell/codec/signature.h"

#include <errno.h>
#include <psa/crypto.h>

bool inkwell_signature_available(void) {
    return true;
}

int inkwell_signature_verify_p256(const uint8_t key[INKWELL_P256_PUBLIC_KEY_LEN],
                                  const uint8_t digest[INKWELL_SHA256_DIGEST_LEN],
                                  const uint8_t signature[INKWELL_P256_SIGNATURE_LEN]) {
    /* Idempotent: the first caller starts the library, every later one is told it is up. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -EIO;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256U);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    if (psa_import_key(&attributes, key, INKWELL_P256_PUBLIC_KEY_LEN, &id) != PSA_SUCCESS) {
        return -EINVAL;
    }
    const psa_status_t status =
        psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, INKWELL_SHA256_DIGEST_LEN,
                        signature, INKWELL_P256_SIGNATURE_LEN);
    (void)psa_destroy_key(id);
    if (status == PSA_SUCCESS) {
        return 0;
    }
    return status == PSA_ERROR_INVALID_SIGNATURE ? -EBADMSG : -EIO;
}
