#define _POSIX_C_SOURCE 200809L

/* SHA-256 against the published vectors. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

INKWELL_TEST_CASE(sha256_vectors, unit) {
    /* The published FIPS 180-4 vectors, plus the empty string. */
    static const struct {
        const char *input;
        const char *expected;
    } k_vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (size_t i = 0; i < sizeof k_vectors / sizeof k_vectors[0]; ++i) {
        struct inkwell_sha256 ctx;
        uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
        char hex[INKWELL_SHA256_HEX_LEN];
        inkwell_sha256_init(&ctx);
        inkwell_sha256_update(&ctx, k_vectors[i].input, strlen(k_vectors[i].input));
        inkwell_sha256_final(&ctx, digest);
        inkwell_sha256_hex(digest, hex, sizeof hex);
        INKWELL_TEST_FAIL_IF(strcmp(hex, k_vectors[i].expected) != 0, hex);
    }

    /* A message longer than one block, fed in awkward pieces: the streaming path and the
       one-shot path must agree, or a download hashed in 4 KB reads would not match. */
    char long_input[1000];
    for (size_t i = 0; i < sizeof long_input; ++i) {
        long_input[i] = (char)('a' + (i % 26U));
    }
    struct inkwell_sha256 whole;
    uint8_t whole_digest[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_init(&whole);
    inkwell_sha256_update(&whole, long_input, sizeof long_input);
    inkwell_sha256_final(&whole, whole_digest);

    struct inkwell_sha256 pieces;
    uint8_t pieces_digest[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_init(&pieces);
    for (size_t offset = 0; offset < sizeof long_input;) {
        const size_t chunk = (offset % 7U) + 1U;
        const size_t take = offset + chunk > sizeof long_input ? sizeof long_input - offset : chunk;
        inkwell_sha256_update(&pieces, long_input + offset, take);
        offset += take;
    }
    inkwell_sha256_final(&pieces, pieces_digest);
    INKWELL_TEST_FAIL_IF(memcmp(whole_digest, pieces_digest, sizeof whole_digest) != 0,
                         "streamed and one-shot hashes should agree");

    /* And the file path, which is what actually verifies a download. */
    char path[] = "/tmp/meshclient_sha256_XXXXXX";
    const int fd = mkstemp(path);
    INKWELL_TEST_FAIL_IF(fd < 0, "could not create a temporary file");
    if (write(fd, "abc", 3U) != 3) {
        close(fd);
        unlink(path);
        record_failure(test_name, "could not write the temporary file");
        return;
    }
    close(fd);
    uint8_t file_digest[INKWELL_SHA256_DIGEST_LEN];
    const int hashed = inkwell_sha256_file(path, file_digest);
    char file_hex[INKWELL_SHA256_HEX_LEN];
    inkwell_sha256_hex(file_digest, file_hex, sizeof file_hex);
    unlink(path);
    if (hashed != 0 ||
        strcmp(file_hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        record_failure(test_name, "hashing a file should match the same bytes in memory");
        return;
    }
    INKWELL_TEST_FAIL_IF(inkwell_sha256_file("/nonexistent/meshclient", file_digest) == 0,
                         "hashing a missing file should fail");
    record_success(test_name);
}
