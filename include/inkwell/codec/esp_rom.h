#pragma once

/*
 * The serial protocol an ESP32's ROM bootloader speaks in download mode - the one esptool
 * speaks - as bytes in and bytes out.
 *
 * Every ESP32 has this bootloader in mask ROM, so it is there whatever the flash holds: a
 * blank chip, a chip running anything at all, a chip whose last write was interrupted. That
 * is what makes it the one way onto the flash that never depends on the application on it.
 * Getting the chip into download mode is a matter of two control lines, not of this codec -
 * see `inkwell_serial_set_lines()` in `io/serial.h`.
 *
 * The wire is SLIP-framed (RFC 1055: 0xC0 delimits, 0xDB escapes) packets, stop-and-wait:
 *
 *   request   0x00, op, u16 size, u32 checksum, `size` bytes of data
 *   response  0x01, op, u16 size, u32 value,    `size` bytes of data ending in the status
 *
 * All little-endian. The checksum is only meaningful on FLASH_DATA, where it is 0xEF XORed
 * over the block. A response's status is the *fourth-last* byte of its data and the error the
 * third-last: the ROMs of the ESP32 family end every answer with four status bytes, where the
 * ESP8266's ROM and esptool's RAM stub end theirs with two. Only the ROM is spoken here - no
 * stub is uploaded - so four it is.
 *
 * What the ROM does *not* do, and why this codec does not pretend it does:
 *
 * - **It takes FLASH_DATA 1 KB at a time.** INKWELL_ESP_ROM_BLOCK is the ROM's limit, not a
 *   choice; a Heltec V3 (ESP32-S3) answers 16 KB blocks with error 0x05. The block is written
 *   before it is answered, so throughput is set by the flash, not the baud rate - measured on
 *   that board, 644 KB took 37 s at 460800 and 37 s at 921600.
 * - **FLASH_BEGIN erases before it answers**, the whole region, so its answer takes as long as
 *   erasing that much flash does (about 3 s a megabyte).
 * - **SPI_FLASH_MD5 answers in hex text**, 32 ASCII digits rather than 16 bytes - the ROM's
 *   spelling, where the stub's is binary. `inkwell_esp_rom_md5_matches()` reads that.
 *
 * Pure: no descriptor, no clock. The conversation - which request follows which answer, and
 * how long to wait for it - is the caller's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ROM's FLASH_DATA block. */
#define INKWELL_ESP_ROM_BLOCK 0x400U
/* The largest request a builder here writes: a FLASH_DATA block with every byte escaped. */
#define INKWELL_ESP_ROM_REQUEST_MAX (2U + 2U * (8U + 16U + INKWELL_ESP_ROM_BLOCK))
/* The largest response kept whole. The longest the ROM sends is the MD5's 44 bytes. */
#define INKWELL_ESP_ROM_FRAME_MAX 128U
/* Where every ESP32-family chip keeps the word that says which chip it is. */
#define INKWELL_ESP_ROM_CHIP_MAGIC_REG 0x40001000U
/* The rate a ROM listens at out of reset, before CHANGE_BAUDRATE. */
#define INKWELL_ESP_ROM_BAUD 115200U

enum inkwell_esp_rom_op {
    INKWELL_ESP_ROM_FLASH_BEGIN = 0x02,
    INKWELL_ESP_ROM_FLASH_DATA = 0x03,
    INKWELL_ESP_ROM_FLASH_END = 0x04,
    INKWELL_ESP_ROM_SYNC = 0x08,
    INKWELL_ESP_ROM_READ_REG = 0x0A,
    INKWELL_ESP_ROM_SPI_SET_PARAMS = 0x0B,
    INKWELL_ESP_ROM_SPI_ATTACH = 0x0D,
    INKWELL_ESP_ROM_CHANGE_BAUDRATE = 0x0F,
    INKWELL_ESP_ROM_SPI_FLASH_MD5 = 0x13,
};

/*
 * Each builder writes one SLIP-framed request into `out` and returns its length, or -ENOBUFS
 * when `out_len` is short (INKWELL_ESP_ROM_REQUEST_MAX never is), or -EINVAL for an argument
 * the ROM would refuse anyway.
 */

/* SYNC, the 36 bytes the ROM tunes its baud detection on. Send it until one is answered. */
int inkwell_esp_rom_sync(uint8_t *out, size_t out_len);
int inkwell_esp_rom_read_reg(uint32_t address, uint8_t *out, size_t out_len);
/* Attaches the SPI flash the chip boots from - its default pins, as a ROM's own boot does. */
int inkwell_esp_rom_spi_attach(uint8_t *out, size_t out_len);
/* Tells the ROM how large the flash is, which is all its bounds checks go by. */
int inkwell_esp_rom_spi_set_params(uint32_t flash_size, uint8_t *out, size_t out_len);
int inkwell_esp_rom_change_baud(uint32_t baud, uint8_t *out, size_t out_len);
/*
 * FLASH_BEGIN for `size` bytes at `offset`, in INKWELL_ESP_ROM_BLOCK blocks. `chip` is an
 * `INKWELL_ESP_CHIP_*` from codec/esp_image.h, and matters: every ROM after the original
 * ESP32's takes a fifth word (flash encryption, always off here), and the original's refuses a
 * request carrying one.
 */
int inkwell_esp_rom_flash_begin(uint16_t chip, uint32_t size, uint32_t offset, uint8_t *out,
                                size_t out_len);
/* FLASH_DATA block `sequence`, `len` bytes of it, padded with 0xFF to a whole block - which is
   what erased flash reads as, so the padding writes nothing. -EINVAL above a block. */
int inkwell_esp_rom_flash_data(uint32_t sequence, const uint8_t *block, size_t len, uint8_t *out,
                               size_t out_len);
/* FLASH_END. `run` leaves download mode and starts the application from flash. */
int inkwell_esp_rom_flash_end(bool run, uint8_t *out, size_t out_len);
int inkwell_esp_rom_flash_md5(uint32_t offset, uint32_t size, uint8_t *out, size_t out_len);

/* How many INKWELL_ESP_ROM_BLOCK blocks FLASH_BEGIN promises for `size` bytes. */
uint32_t inkwell_esp_rom_blocks(uint32_t size);

/*
 * Assembles responses out of whatever the port delivers.
 *
 * A port in download mode also carries text - the ROM's boot banner, the application's log
 * right up to the reset - and a read can start in the middle of a frame. So anything outside
 * a delimiter pair is ignored, and a delimited run that does not parse as a response is taken
 * to have been the *gap between* two frames: its closing 0xC0 opens the next one, which is how
 * a reader that joined mid-frame falls into step by the following answer.
 */
struct inkwell_esp_rom_reader {
    uint8_t frame[INKWELL_ESP_ROM_FRAME_MAX];
    size_t len;
    bool in_frame;
    bool escape;
    bool overflow;
};

struct inkwell_esp_rom_response {
    uint8_t op;
    uint32_t value;
    /* Points into the reader's frame: valid until the next feed. Includes the status bytes. */
    const uint8_t *data;
    size_t data_len;
    /* 0 is success; anything else is refused, and `error` says why. */
    uint8_t status;
    uint8_t error;
};

void inkwell_esp_rom_reader_reset(struct inkwell_esp_rom_reader *reader);

/*
 * Consumes `data` up to and including the end of the first complete response, which it
 * decodes into `out` and reports with `*ready`. Returns how many bytes it consumed: feed the
 * rest again, since a read can carry more than one answer.
 */
size_t inkwell_esp_rom_reader_feed(struct inkwell_esp_rom_reader *reader, const uint8_t *data,
                                   size_t len, struct inkwell_esp_rom_response *out, bool *ready);

/* The chip a READ_REG of INKWELL_ESP_ROM_CHIP_MAGIC_REG names, as an `INKWELL_ESP_CHIP_*`.
   False for a magic this codec does not know. */
bool inkwell_esp_rom_chip_for_magic(uint32_t magic, uint16_t *out_chip);

/* True when an SPI_FLASH_MD5 answer carries `digest`. */
bool inkwell_esp_rom_md5_matches(const struct inkwell_esp_rom_response *response,
                                 const uint8_t digest[16]);

/* The ROM's name for a response's `error`. Never NULL. */
const char *inkwell_esp_rom_error_name(uint8_t error);

#ifdef __cplusplus
}
#endif
