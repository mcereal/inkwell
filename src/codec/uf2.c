#include "inkwell/codec/uf2.h"

#include <string.h>

/* Little-endian on the wire, read byte by byte: a UF2 comes off a network and out of a zip,
   so nothing here may depend on the host's alignment or byte order. */
static uint32_t uf2_u32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

bool inkwell_uf2_block_parse(const uint8_t *bytes, size_t len, struct inkwell_uf2_block *out) {
    if (bytes == NULL || out == NULL || len < INKWELL_UF2_BLOCK_SIZE) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (uf2_u32(bytes) != INKWELL_UF2_MAGIC_START0 ||
        uf2_u32(bytes + 4U) != INKWELL_UF2_MAGIC_START1 ||
        uf2_u32(bytes + INKWELL_UF2_BLOCK_SIZE - 4U) != INKWELL_UF2_MAGIC_END) {
        return false;
    }
    out->flags = uf2_u32(bytes + 8U);
    out->target_address = uf2_u32(bytes + 12U);
    out->payload_size = uf2_u32(bytes + 16U);
    out->block_no = uf2_u32(bytes + 20U);
    out->num_blocks = uf2_u32(bytes + 24U);
    /*
     * Word 7 is three different things depending on the flags - a file size, a family id, or
     * nothing - so it is only read as a family when the flag says it is one. Reading it
     * unconditionally is how a file-container UF2's *length* comes to be compared against a
     * chip's family id, and the numbers involved are large enough that the mismatch looks
     * deliberate.
     */
    out->has_family = (out->flags & INKWELL_UF2_FLAG_FAMILY_ID) != 0U;
    out->family_id = out->has_family ? uf2_u32(bytes + 28U) : 0U;
    out->skip = (out->flags & INKWELL_UF2_FLAG_NOT_MAIN_FLASH) != 0U;
    return true;
}

enum inkwell_uf2_verdict inkwell_uf2_validate(const uint8_t *image, size_t len,
                                              uint32_t expect_family,
                                              struct inkwell_uf2_info *out) {
    struct inkwell_uf2_info info;
    memset(&info, 0, sizeof info);
    info.contiguous = true;

    enum inkwell_uf2_verdict verdict = INKWELL_UF2_OK;
    if (image == NULL || len < INKWELL_UF2_BLOCK_SIZE || (len % INKWELL_UF2_BLOCK_SIZE) != 0U) {
        verdict = INKWELL_UF2_NOT_UF2;
    } else {
        const size_t count = len / INKWELL_UF2_BLOCK_SIZE;
        bool addressed = false;
        uint32_t next_address = 0U;
        for (size_t i = 0; i < count; ++i) {
            struct inkwell_uf2_block block;
            if (!inkwell_uf2_block_parse(image + i * INKWELL_UF2_BLOCK_SIZE, INKWELL_UF2_BLOCK_SIZE,
                                         &block)) {
                /* The first block failing means this is not a UF2; a later one failing means
                   it is one and something is wrong with it. Two different verdicts. */
                verdict = i == 0U ? INKWELL_UF2_NOT_UF2 : INKWELL_UF2_MALFORMED;
                break;
            }
            if (block.payload_size > INKWELL_UF2_PAYLOAD_MAX || (block.payload_size & 3U) != 0U) {
                verdict = INKWELL_UF2_MALFORMED;
                break;
            }
            /*
             * A block that runs off the end of the address space is not an image, and it is
             * the one arithmetic here that could wrap: both fields are 32-bit and both come
             * out of the file. Left in, the span a caller computes comes back negative and
             * enormous, which is a progress bar that never moves and a size check that passes.
             */
            if (block.target_address > UINT32_MAX - block.payload_size) {
                verdict = INKWELL_UF2_MALFORMED;
                break;
            }
            if (i == 0U) {
                info.has_family = block.has_family;
                info.family_id = block.family_id;
                info.num_blocks = block.num_blocks;
                /*
                 * The family is checked once, against the first block, and then every block is
                 * checked against the first - so a file that changes family halfway through is
                 * refused whichever end the caller's expectation matched.
                 */
                if (expect_family != 0U &&
                    (!block.has_family || block.family_id != expect_family)) {
                    verdict = INKWELL_UF2_WRONG_FAMILY;
                    break;
                }
            } else if (block.has_family != info.has_family || block.family_id != info.family_id) {
                verdict = INKWELL_UF2_WRONG_FAMILY;
                break;
            } else if (block.num_blocks != info.num_blocks) {
                verdict = INKWELL_UF2_MALFORMED;
                break;
            }
            if (block.block_no != (uint32_t)i) {
                verdict = INKWELL_UF2_OUT_OF_ORDER;
                break;
            }
            info.blocks++;
            if (block.skip) {
                continue;
            }
            if (!addressed) {
                info.first_address = block.target_address;
                addressed = true;
            } else if (block.target_address != next_address) {
                info.contiguous = false;
            }
            next_address = block.target_address + block.payload_size;
            info.last_address = next_address;
            info.payload_bytes += block.payload_size;
        }
        if (verdict == INKWELL_UF2_OK && info.blocks != info.num_blocks) {
            verdict =
                info.blocks < info.num_blocks ? INKWELL_UF2_INCOMPLETE : INKWELL_UF2_MALFORMED;
        }
    }

    if (out != NULL) {
        *out = info;
    }
    return verdict;
}
