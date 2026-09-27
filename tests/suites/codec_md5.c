/* MD5 against RFC 1321's own vectors. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/md5.h"

#include <string.h>

INKWELL_TEST_CASE(md5_vectors, unit) {
    static const struct {
        const char *input;
        const char *expected;
    } k_vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (size_t i = 0; i < sizeof k_vectors / sizeof k_vectors[0]; ++i) {
        struct inkwell_md5 ctx;
        uint8_t digest[INKWELL_MD5_DIGEST_LEN];
        char hex[INKWELL_MD5_HEX_LEN];
        inkwell_md5_init(&ctx);
        inkwell_md5_update(&ctx, k_vectors[i].input, strlen(k_vectors[i].input));
        inkwell_md5_final(&ctx, digest);
        inkwell_md5_hex(digest, hex, sizeof hex);
        INKWELL_TEST_FAIL_IF(strcmp(hex, k_vectors[i].expected) != 0, hex);
    }
}

INKWELL_TEST_CASE(md5_in_pieces_matches_whole, unit) {
    /* A region hashed as it is sent, a block at a time, must match one hashed in one call -
       and the 55/56/64-byte edges are where the padding goes wrong. */
    uint8_t message[1000];
    for (size_t i = 0; i < sizeof message; ++i) {
        message[i] = (uint8_t)(i * 7U);
    }
    static const size_t k_lengths[] = {55U, 56U, 63U, 64U, 65U, 1000U};
    for (size_t n = 0; n < sizeof k_lengths / sizeof k_lengths[0]; ++n) {
        struct inkwell_md5 whole;
        struct inkwell_md5 pieces;
        uint8_t a[INKWELL_MD5_DIGEST_LEN];
        uint8_t b[INKWELL_MD5_DIGEST_LEN];
        inkwell_md5_init(&whole);
        inkwell_md5_update(&whole, message, k_lengths[n]);
        inkwell_md5_final(&whole, a);
        inkwell_md5_init(&pieces);
        for (size_t at = 0; at < k_lengths[n]; at += 13U) {
            const size_t left = k_lengths[n] - at;
            inkwell_md5_update(&pieces, message + at, left < 13U ? left : 13U);
        }
        inkwell_md5_final(&pieces, b);
        INKWELL_TEST_FAIL_IF(memcmp(a, b, sizeof a) != 0, "pieces and whole disagree");
    }
}
