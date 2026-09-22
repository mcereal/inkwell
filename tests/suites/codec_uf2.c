/*
 * Reading a UF2, against a real one.
 *
 * `tests/data/t114_2.7.26.uf2` is the first two and last two blocks of the T114's 2.7.26
 * image, cut out of the file the release zip actually holds and otherwise untouched - so the
 * numbers below are the served file's, and the fixture is 2 KB instead of 1.4 MB. Keeping both
 * ends rather than a prefix is deliberate: it pins where the image starts *and* where it ends,
 * and it gives the suite both of the ways a download goes wrong for free. Its first half is a
 * truncated file - two real blocks declaring 2,866 - and the whole of it is a file missing its
 * middle, which the third block says by being numbered 2,864 where 2 was due.
 *
 * A whole file is tested too, and it is derived from those bytes rather than written: block 0
 * with its `numBlocks` rewritten to 1 is a legal single-block UF2 whose every other field came
 * off the wire.
 */

#include "framework/inkwell_test.h"
#include "support/data_fixture.h"

#include "inkwell/codec/uf2.h"

#include <stdlib.h>
#include <string.h>

/* Change only the little-endian numBlocks field in a captured block. */
static void uf2_set_num_blocks(uint8_t *block, uint32_t value) {
    block[24] = (uint8_t)(value & 0xFFU);
    block[25] = (uint8_t)((value >> 8) & 0xFFU);
    block[26] = (uint8_t)((value >> 16) & 0xFFU);
    block[27] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Every field of a real block, pinned.
 *
 * `payload_size` is the one worth stating out loud: 256, not the 476 a block may carry, so an
 * arithmetic written against the maximum is wrong by 46% and still looks plausible.
 */
INKWELL_TEST_CASE(uf2_reads_a_release_block, unit) {
    size_t len = 0U;
    char *const bytes = inkwell_test_data_read("t114_2.7.26.uf2", &len);
    INKWELL_TEST_FAIL_IF(bytes == NULL, "tests/data/t114_2.7.26.uf2 should be readable");
    INKWELL_TEST_FAIL_IF_CLEANUP(len != 4U * INKWELL_UF2_BLOCK_SIZE, free(bytes),
                                 "the fixture is four 512-byte blocks");

    struct inkwell_uf2_block first;
    INKWELL_TEST_FAIL_IF_CLEANUP(
        !inkwell_uf2_block_parse((const uint8_t *)bytes, INKWELL_UF2_BLOCK_SIZE, &first),
        free(bytes), "the first block should carry the magic");
    INKWELL_TEST_FAIL_IF_CLEANUP(first.num_blocks != 2866U, free(bytes),
                                 "the 2.7.26 T114 image is 2,866 blocks");
    INKWELL_TEST_FAIL_IF_CLEANUP(first.block_no != 0U, free(bytes), "numbered from zero");
    INKWELL_TEST_FAIL_IF_CLEANUP(first.payload_size != 256U, free(bytes),
                                 "carrying 256 payload bytes, not the 476 a block may hold");
    INKWELL_TEST_FAIL_IF_CLEANUP(first.target_address != 0x26000U, free(bytes),
                                 "starting above the SoftDevice at 0x26000");
    INKWELL_TEST_FAIL_IF_CLEANUP(!first.has_family ||
                                     first.family_id != INKWELL_UF2_FAMILY_NRF52840,
                                 free(bytes), "and naming the nRF52840 in its own block");
    INKWELL_TEST_FAIL_IF_CLEANUP(first.skip, free(bytes), "it is a main-flash block");

    /* The last block of the real file, which the cut kept. Its address is where the fixture's
       measurement said the image ends. */
    struct inkwell_uf2_block last;
    INKWELL_TEST_FAIL_IF_CLEANUP(
        !inkwell_uf2_block_parse((const uint8_t *)bytes + 3U * INKWELL_UF2_BLOCK_SIZE,
                                 INKWELL_UF2_BLOCK_SIZE, &last),
        free(bytes), "the last block should parse too");
    INKWELL_TEST_FAIL_IF_CLEANUP(last.block_no != 2865U, free(bytes),
                                 "and be the 2,866th, counted from zero");
    INKWELL_TEST_FAIL_IF_CLEANUP(last.target_address != 0xD9100U, free(bytes), "ending at 0xD9100");
    free(bytes);
    record_success(test_name);
}

/*
 * A file with fewer blocks than it says it has is a truncated download, and says so.
 *
 * The first half of the fixture is exactly that: the real file's first two blocks, still
 * declaring 2,866. Nothing about the two is wrong, which is the point - a reader that only
 * checked the blocks it was given would call this good and hand 1 KB to a bootloader expecting
 * 1.4 MB.
 *
 * The fixture *whole* is a different fault and gets its own answer. It is a splice, not a
 * prefix - blocks 0, 1, 2864, 2865 - so what is missing is the middle, and the third block
 * says so by being numbered 2,864 where 2 was due. That is the more useful verdict of the two:
 * "it did not all arrive" and "it did not arrive in one piece" are different things to have
 * to explain to a user.
 */
INKWELL_TEST_CASE(uf2_refuses_a_truncated_image, unit) {
    size_t len = 0U;
    char *const bytes = inkwell_test_data_read("t114_2.7.26.uf2", &len);
    INKWELL_TEST_FAIL_IF(bytes == NULL, "the fixture should be readable");

    struct inkwell_uf2_info info;
    const enum inkwell_uf2_verdict cut = inkwell_uf2_validate(
        (const uint8_t *)bytes, 2U * INKWELL_UF2_BLOCK_SIZE, INKWELL_UF2_FAMILY_NRF52840, &info);
    INKWELL_TEST_FAIL_IF_CLEANUP(cut != INKWELL_UF2_INCOMPLETE, free(bytes),
                                 "two blocks of 2,866 is an incomplete file");
    /* The report is still worth reading after a refusal: it says how far it got. */
    INKWELL_TEST_FAIL_IF_CLEANUP(info.blocks != 2U || info.num_blocks != 2866U, free(bytes),
                                 "and it says how many arrived against how many were declared");

    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_uf2_validate((const uint8_t *)bytes, len, INKWELL_UF2_FAMILY_NRF52840, &info) !=
            INKWELL_UF2_OUT_OF_ORDER,
        free(bytes), "while a file missing its middle is refused as a broken sequence");
    INKWELL_TEST_FAIL_IF_CLEANUP(info.blocks != 2U, free(bytes),
                                 "reported at the block where the sequence broke");

    /* A length that is not a whole number of blocks is not a UF2 at all - a different verdict. */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_uf2_validate((const uint8_t *)bytes, len - 3U, 0U, NULL) != INKWELL_UF2_NOT_UF2,
        free(bytes), "and a length that is not 512-byte records is not one either");
    free(bytes);
    record_success(test_name);
}

/*
 * A whole file, and the one guard that protects the board rather than the download.
 *
 * The family id is in every block, so there is no header to strip and no handshake to get
 * wrong: an RP2040 image offered to an nRF52840 device is refused by reading it, before anything is
 * written. A bootloader refuses the wrong family too - this reports the mismatch before any write.
 */
INKWELL_TEST_CASE(uf2_refuses_another_chips_image, unit) {
    size_t len = 0U;
    char *const bytes = inkwell_test_data_read("t114_2.7.26.uf2", &len);
    INKWELL_TEST_FAIL_IF(bytes == NULL, "the fixture should be readable");

    /* Block 0 alone, told the truth about how long the file now is. */
    uint8_t whole[INKWELL_UF2_BLOCK_SIZE];
    memcpy(whole, bytes, sizeof whole);
    free(bytes);
    uf2_set_num_blocks(whole, 1U);

    struct inkwell_uf2_info info;
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(whole, sizeof whole, INKWELL_UF2_FAMILY_NRF52840,
                                              &info) != INKWELL_UF2_OK,
                         "a complete single-block image should validate");
    INKWELL_TEST_FAIL_IF(info.blocks != 1U || info.payload_bytes != 256U,
                         "and report one block of 256 payload bytes");
    INKWELL_TEST_FAIL_IF(!info.contiguous, "a single block is trivially contiguous");
    INKWELL_TEST_FAIL_IF(info.first_address != 0x26000U || info.last_address != 0x26100U,
                         "spanning the 256 bytes it carries");

    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(whole, sizeof whole, INKWELL_UF2_FAMILY_RP2040,
                                              NULL) != INKWELL_UF2_WRONG_FAMILY,
                         "the same file offered as an RP2040 image is refused");
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(whole, sizeof whole, INKWELL_UF2_FAMILY_RP2350,
                                              NULL) != INKWELL_UF2_WRONG_FAMILY,
                         "and so is an RP2350 one, which differs from the RP2040 by three bits");
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(whole, sizeof whole, 0U, NULL) != INKWELL_UF2_OK,
                         "an expectation of 0 accepts whatever the file says it is");
    record_success(test_name);
}

/* Blocks in the wrong order, and blocks that are not blocks. Both are files a bootloader would
   accept some of, which is why they are refused here rather than there. */
INKWELL_TEST_CASE(uf2_refuses_a_file_that_is_not_a_sequence, unit) {
    size_t len = 0U;
    char *const bytes = inkwell_test_data_read("t114_2.7.26.uf2", &len);
    INKWELL_TEST_FAIL_IF(bytes == NULL, "the fixture should be readable");

    /* Blocks 0 and 1 of the real file, declared as a two-block image: valid. */
    uint8_t pair[2U * INKWELL_UF2_BLOCK_SIZE];
    memcpy(pair, bytes, sizeof pair);
    free(bytes);
    uf2_set_num_blocks(pair, 2U);
    uf2_set_num_blocks(pair + INKWELL_UF2_BLOCK_SIZE, 2U);
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(pair, sizeof pair, INKWELL_UF2_FAMILY_NRF52840,
                                              NULL) != INKWELL_UF2_OK,
                         "two consecutive real blocks are a valid two-block image");

    /* One byte of the second block's magic, and it stops being a block. Mid-file that is a
       malformed UF2 rather than "this is not a UF2", because the first block said it was. */
    uint8_t broken[sizeof pair];
    memcpy(broken, pair, sizeof broken);
    broken[INKWELL_UF2_BLOCK_SIZE] ^= 0xFFU;
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(broken, sizeof broken, 0U, NULL) !=
                             INKWELL_UF2_MALFORMED,
                         "a block that lost its magic mid-file is malformed");

    memcpy(broken, pair, sizeof broken);
    broken[0] ^= 0xFFU;
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(broken, sizeof broken, 0U, NULL) !=
                             INKWELL_UF2_NOT_UF2,
                         "and one that lost it at the front is not a UF2 at all");

    /* The two real blocks, swapped. Every field is genuine and the sequence is not. */
    uint8_t swapped[sizeof pair];
    memcpy(swapped, pair + INKWELL_UF2_BLOCK_SIZE, INKWELL_UF2_BLOCK_SIZE);
    memcpy(swapped + INKWELL_UF2_BLOCK_SIZE, pair, INKWELL_UF2_BLOCK_SIZE);
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(swapped, sizeof swapped, 0U, NULL) !=
                             INKWELL_UF2_OUT_OF_ORDER,
                         "blocks out of order are refused");

    /* A payload longer than a block can hold. The field is self-declared, which is the shape
       that goes wrong. */
    memcpy(broken, pair, sizeof broken);
    broken[16] = 0xFFU;
    broken[17] = 0x01U;
    INKWELL_TEST_FAIL_IF(inkwell_uf2_validate(broken, sizeof broken, 0U, NULL) !=
                             INKWELL_UF2_MALFORMED,
                         "and so is a payload longer than the block carrying it");
    record_success(test_name);
}
