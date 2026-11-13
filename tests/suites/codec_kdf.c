/* HMAC-SHA256 against RFC 4231's vectors and PBKDF2-HMAC-SHA256 against RFC 7914's and Python's
   hashlib.pbkdf2_hmac, an implementation written apart from this one. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/kdf.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void hex(const uint8_t *bytes, size_t len, char *out) {
    for (size_t i = 0U; i < len; ++i) {
        snprintf(out + 2U * i, 3U, "%02x", bytes[i]);
    }
}

INKWELL_TEST_CASE(kdf_hmac_sha256_matches_rfc_4231, unit) {
    uint8_t key[131];
    uint8_t mac[INKWELL_HMAC_SHA256_LEN];
    char text[2U * INKWELL_HMAC_SHA256_LEN + 1U];
    memset(key, 0x0B, 20U);
    inkwell_hmac_sha256(key, 20U, "Hi There", 8U, mac);
    hex(mac, sizeof mac, text);
    INKWELL_TEST_FAIL_IF(
        strcmp(text, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") != 0,
        "test case 1");
    memset(key, 0xAA, sizeof key);
    static const char k_msg[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    inkwell_hmac_sha256(key, sizeof key, k_msg, sizeof k_msg - 1U, mac);
    hex(mac, sizeof mac, text);
    INKWELL_TEST_FAIL_IF(
        strcmp(text, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") != 0,
        "test case 6: a key longer than a block is hashed first");
    record_success(test_name);
}

INKWELL_TEST_CASE(kdf_pbkdf2_sha256_matches_its_references, unit) {
    uint8_t out[64];
    char text[129];
    INKWELL_TEST_FAIL_IF(inkwell_pbkdf2_sha256("passwd", 6U, "salt", 4U, 1U, out, 64U) != 0,
                         "derives");
    hex(out, 64U, text);
    INKWELL_TEST_FAIL_IF(strcmp(text, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20"
                                      "dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd5091120"
                                      "41d3a19783") != 0,
                         "RFC 7914 section 11: two blocks, one round");
    INKWELL_TEST_FAIL_IF(inkwell_pbkdf2_sha256("password", 8U, "salt", 4U, 4096U, out, 32U) != 0,
                         "derives");
    hex(out, 32U, text);
    INKWELL_TEST_FAIL_IF(
        strcmp(text, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a") != 0,
        "4096 rounds");
    uint8_t salt[16];
    for (size_t i = 0U; i < sizeof salt; ++i) {
        salt[i] = (uint8_t)i;
    }
    INKWELL_TEST_FAIL_IF(inkwell_pbkdf2_sha256("1234", 4U, salt, sizeof salt, 1000U, out, 32U) != 0,
                         "derives");
    hex(out, 32U, text);
    INKWELL_TEST_FAIL_IF(
        strcmp(text, "2240ea2a22754a3c4610356ef8e4d7acf25af2ac3d2d11866de97a8b7812c52c") != 0,
        "a binary salt");
    INKWELL_TEST_FAIL_IF(inkwell_pbkdf2_sha256("x", 1U, "s", 1U, 0U, out, 32U) != -EINVAL ||
                             inkwell_pbkdf2_sha256("x", 1U, "s", 1U, 1U, out, 0U) != -EINVAL,
                         "no rounds or no output is refused");
    record_success(test_name);
}

INKWELL_TEST_CASE(kdf_equal_secret_compares_every_byte, unit) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 5};
    INKWELL_TEST_FAIL_IF(!inkwell_equal_secret(a, a, 4U), "the same");
    INKWELL_TEST_FAIL_IF(inkwell_equal_secret(a, b, 4U), "differing in the last byte");
    INKWELL_TEST_FAIL_IF(!inkwell_equal_secret(a, b, 3U), "only the length asked is compared");
    record_success(test_name);
}
