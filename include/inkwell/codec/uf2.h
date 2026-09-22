#pragma once

/*
 * Reading a UF2 image before handing its blocks to a bootloader.
 *
 * A UF2 is 512-byte records and nothing else: no header, no index, no trailer. Each record
 * carries its own address, its own position in the file and the file's own length, which is
 * what makes the format writable to a device that has no idea what a filesystem is. The
 * Adafruit nRF52 bootloader really does ignore the FAT it presents, watch every 512-byte block
 * that goes past, and flash the ones carrying the magic - so a `.uf2` written to its mass
 * storage in any order, through any layer, arrives.
 *
 * When bytes are their own protocol there is no envelope to get wrong and no handshake
 * to fail: the defence against writing an image for another chip is reading what the
 * blocks say before writing them. The family id is that
 * check, it is in **every** block rather than in a header somebody could strip, and it is
 * hardware-enforced on the far side too - a bootloader refuses a family that is not its own.
 * Checking it here reports a mismatch before any block is written.
 *
 * Pure: bytes in, verdict out, no file handles and no device.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed, and it is the whole of the format's framing. */
#define INKWELL_UF2_BLOCK_SIZE 512U
/*
 * The most one block *may* carry. The captured release image carries **256**, and
 * pinning that matters more than it looks: an arithmetic written against the maximum is wrong
 * by 46% and still produces a plausible-looking number - a progress bar that reaches 54% and
 * stops, or a size check that passes a file half the length it should be.
 */
#define INKWELL_UF2_PAYLOAD_MAX 476U

#define INKWELL_UF2_MAGIC_START0 0x0A324655U /* "UF2\n" */
#define INKWELL_UF2_MAGIC_START1 0x9E5D5157U
#define INKWELL_UF2_MAGIC_END 0x0AB16F30U

/* This block is not for the main flash - a bootloader skips it, and so do we. */
#define INKWELL_UF2_FLAG_NOT_MAIN_FLASH 0x00000001U
/* `file_size` is a family id rather than a length. Set on every release image measured. */
#define INKWELL_UF2_FLAG_FAMILY_ID 0x00002000U

/*
 * Family ids, **measured** off real 2.7.26 release images rather than read off a table: the
 * nrf52840 zip's T114 `.uf2`, the rp2040 zip's `rp2040-lora`, and the rp2350 zip's `pico2w`.
 * The RP2350 has more than one published family - secure, non-secure, RISC-V. The
 * captured image uses the ARM secure family; a caller must match the family actually
 * carried by its image to the intended device.
 */
#define INKWELL_UF2_FAMILY_NRF52840 0xADA52840U
#define INKWELL_UF2_FAMILY_RP2040 0xE48BFF56U
#define INKWELL_UF2_FAMILY_RP2350 0xE48BFF59U

/*
 * Why a file was refused. Each names a distinct failure, so callers can report the precise reason.
 */
enum inkwell_uf2_verdict {
    INKWELL_UF2_OK = 0,
    /* Not a length that could be 512-byte records, or the first record has no magic. */
    INKWELL_UF2_NOT_UF2,
    /* Records, but one of them is wrong: no magic mid-file, an oversized or unaligned payload,
       an inconsistent `numBlocks`, or more records than declared. */
    INKWELL_UF2_MALFORMED,
    /* A UF2 for a different chip. The family does not match the caller's expected chip. */
    INKWELL_UF2_WRONG_FAMILY,
    /* `blockNo` is not the sequence 0..n-1. Every generator writes them in order and the
       bootloader counts on it; a file that is not is one we cannot report progress against. */
    INKWELL_UF2_OUT_OF_ORDER,
    /* Fewer records than the records themselves say there should be - a truncated download. */
    INKWELL_UF2_INCOMPLETE,
    INKWELL_UF2_VERDICT_COUNT,
};

/* One 512-byte record, as it describes itself. */
struct inkwell_uf2_block {
    uint32_t flags;
    uint32_t target_address;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    /* Only meaningful with `has_family`; 0 otherwise, which is also a legal family id, hence
       the flag rather than a sentinel. */
    uint32_t family_id;
    bool has_family;
    /* INKWELL_UF2_FLAG_NOT_MAIN_FLASH, pulled out because it is the one flag that changes what a
       caller does with the block. */
    bool skip;
};

/* What a whole file turned out to be. Filled in as far as the walk got, so it is still worth
   reading after a verdict that is not OK. */
struct inkwell_uf2_info {
    uint32_t family_id;
    bool has_family;
    /* What the records claim, and how many were actually there. Equal on a whole file. */
    uint32_t num_blocks;
    uint32_t blocks;
    /* Payload bytes across every block that is not a skip: what will actually be flashed, and
       not the same number as the file's length. */
    uint64_t payload_bytes;
    uint32_t first_address;
    /* The address one past the last payload byte, so `last_address - first_address` is the
       span - which equals `payload_bytes` exactly when the file is contiguous. */
    uint32_t last_address;
    /* Every block's address followed the one before it with no gap. True for every release
       image measured; false is not a refusal, because a legal UF2 may skip regions. */
    bool contiguous;
};

/*
 * Reads one 512-byte record. False when it is short or carries no magic - which is not
 * necessarily an error: a block without the magic is discarded by the bootloader and reported
 * as written, which is the property that lets the write path be exercised at full size against
 * a real board without touching its flash.
 */
bool inkwell_uf2_block_parse(const uint8_t *bytes, size_t len, struct inkwell_uf2_block *out);

/*
 * Walks a whole `.uf2` and says whether it is one, and whether it is for `expect_family`.
 *
 * `expect_family` of 0 accepts whatever the file says and reports it for inspection.
 * A caller flashing a device passes its expected family before writing any block.
 *
 * `out` may be NULL. The walk stops at the first block it refuses, so `out->blocks` says where.
 */
enum inkwell_uf2_verdict inkwell_uf2_validate(const uint8_t *image, size_t len,
                                              uint32_t expect_family, struct inkwell_uf2_info *out);

#ifdef __cplusplus
}
#endif
