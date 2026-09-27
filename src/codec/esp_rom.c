#include "inkwell/codec/esp_rom.h"

#include "inkwell/codec/esp_image.h"

#include <errno.h>
#include <string.h>

#define SLIP_END 0xC0U
#define SLIP_ESC 0xDBU
#define SLIP_ESC_END 0xDCU
#define SLIP_ESC_ESC 0xDDU

/* The seed of FLASH_DATA's checksum. */
#define ESP_ROM_CHECKSUM_SEED 0xEFU
#define ESP_ROM_HEADER_LEN 8U
/* What a ROM of the ESP32 family ends every answer with. */
#define ESP_ROM_STATUS_LEN 4U

static void put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/* Appends one byte, escaped. False when it does not fit. */
static bool slip_put(uint8_t *out, size_t out_len, size_t *at, uint8_t byte) {
    if (byte == SLIP_END || byte == SLIP_ESC) {
        if (*at + 2U > out_len) {
            return false;
        }
        out[(*at)++] = SLIP_ESC;
        out[(*at)++] = byte == SLIP_END ? SLIP_ESC_END : SLIP_ESC_ESC;
        return true;
    }
    if (*at + 1U > out_len) {
        return false;
    }
    out[(*at)++] = byte;
    return true;
}

/* One request, framed: the header, then `head` (the op's fixed words), then `body` (a block). */
static int request(uint8_t op, const uint8_t *head, size_t head_len, const uint8_t *body,
                   size_t body_len, uint32_t checksum, uint8_t *out, size_t out_len) {
    if (out == NULL) {
        return -EINVAL;
    }
    const size_t size = head_len + body_len;
    uint8_t header[ESP_ROM_HEADER_LEN] = {0x00U, op, (uint8_t)size, (uint8_t)(size >> 8)};
    put_u32(header + 4, checksum);

    size_t at = 0U;
    if (out_len < 1U) {
        return -ENOBUFS;
    }
    out[at++] = SLIP_END;
    for (size_t i = 0; i < sizeof header; ++i) {
        if (!slip_put(out, out_len, &at, header[i])) {
            return -ENOBUFS;
        }
    }
    for (size_t i = 0; i < head_len; ++i) {
        if (!slip_put(out, out_len, &at, head[i])) {
            return -ENOBUFS;
        }
    }
    for (size_t i = 0; i < body_len; ++i) {
        if (!slip_put(out, out_len, &at, body[i])) {
            return -ENOBUFS;
        }
    }
    if (at + 1U > out_len) {
        return -ENOBUFS;
    }
    out[at++] = SLIP_END;
    return (int)at;
}

static int words(uint8_t op, const uint32_t *values, size_t count, uint8_t *out, size_t out_len) {
    uint8_t head[24];
    for (size_t i = 0; i < count; ++i) {
        put_u32(head + 4U * i, values[i]);
    }
    return request(op, head, 4U * count, NULL, 0U, 0U, out, out_len);
}

int inkwell_esp_rom_sync(uint8_t *out, size_t out_len) {
    uint8_t body[36] = {0x07U, 0x07U, 0x12U, 0x20U};
    memset(body + 4, 0x55, sizeof body - 4U);
    return request(INKWELL_ESP_ROM_SYNC, body, sizeof body, NULL, 0U, 0U, out, out_len);
}

int inkwell_esp_rom_read_reg(uint32_t address, uint8_t *out, size_t out_len) {
    return words(INKWELL_ESP_ROM_READ_REG, &address, 1U, out, out_len);
}

int inkwell_esp_rom_spi_attach(uint8_t *out, size_t out_len) {
    /* The pin configuration word, 0 for the chip's own flash pins, and a word the ROM
       ignores but still counts. */
    const uint32_t values[2] = {0U, 0U};
    return words(INKWELL_ESP_ROM_SPI_ATTACH, values, 2U, out, out_len);
}

int inkwell_esp_rom_spi_set_params(uint32_t flash_size, uint8_t *out, size_t out_len) {
    if (flash_size == 0U) {
        return -EINVAL;
    }
    /* id, total size, block, sector, page, status mask - esptool's values for every chip. */
    const uint32_t values[6] = {0U, flash_size, 64U * 1024U, 4U * 1024U, 256U, 0xFFFFU};
    return words(INKWELL_ESP_ROM_SPI_SET_PARAMS, values, 6U, out, out_len);
}

int inkwell_esp_rom_change_baud(uint32_t baud, uint8_t *out, size_t out_len) {
    if (baud == 0U) {
        return -EINVAL;
    }
    /* The second word is the rate being left, which only the stub reads; the ROM wants 0. */
    const uint32_t values[2] = {baud, 0U};
    return words(INKWELL_ESP_ROM_CHANGE_BAUDRATE, values, 2U, out, out_len);
}

uint32_t inkwell_esp_rom_blocks(uint32_t size) {
    return (uint32_t)(((uint64_t)size + INKWELL_ESP_ROM_BLOCK - 1U) / INKWELL_ESP_ROM_BLOCK);
}

int inkwell_esp_rom_flash_begin(uint16_t chip, uint32_t size, uint32_t offset, uint8_t *out,
                                size_t out_len) {
    if (size == 0U) {
        return -EINVAL;
    }
    const uint32_t values[5] = {size, inkwell_esp_rom_blocks(size), INKWELL_ESP_ROM_BLOCK, offset,
                                0U};
    return words(INKWELL_ESP_ROM_FLASH_BEGIN, values, chip == INKWELL_ESP_CHIP_ESP32 ? 4U : 5U, out,
                 out_len);
}

int inkwell_esp_rom_flash_data(uint32_t sequence, const uint8_t *block, size_t len, uint8_t *out,
                               size_t out_len) {
    if ((block == NULL && len > 0U) || len > INKWELL_ESP_ROM_BLOCK) {
        return -EINVAL;
    }
    uint8_t padded[INKWELL_ESP_ROM_BLOCK];
    if (len > 0U) {
        memcpy(padded, block, len);
    }
    memset(padded + len, 0xFF, sizeof padded - len);

    uint8_t checksum = ESP_ROM_CHECKSUM_SEED;
    for (size_t i = 0; i < sizeof padded; ++i) {
        checksum ^= padded[i];
    }
    uint8_t head[16];
    put_u32(head, INKWELL_ESP_ROM_BLOCK);
    put_u32(head + 4, sequence);
    put_u32(head + 8, 0U);
    put_u32(head + 12, 0U);
    return request(INKWELL_ESP_ROM_FLASH_DATA, head, sizeof head, padded, sizeof padded, checksum,
                   out, out_len);
}

int inkwell_esp_rom_flash_end(bool run, uint8_t *out, size_t out_len) {
    /* The ROM's flag is "stay": 0 runs the application, 1 stays in download mode. */
    const uint32_t stay = run ? 0U : 1U;
    return words(INKWELL_ESP_ROM_FLASH_END, &stay, 1U, out, out_len);
}

int inkwell_esp_rom_flash_md5(uint32_t offset, uint32_t size, uint8_t *out, size_t out_len) {
    if (size == 0U) {
        return -EINVAL;
    }
    const uint32_t values[4] = {offset, size, 0U, 0U};
    return words(INKWELL_ESP_ROM_SPI_FLASH_MD5, values, 4U, out, out_len);
}

/* ---- responses ------------------------------------------------------------------------ */

void inkwell_esp_rom_reader_reset(struct inkwell_esp_rom_reader *reader) {
    if (reader != NULL) {
        memset(reader, 0, sizeof *reader);
    }
}

/* A delimited run that is a whole response. */
static bool parse(const uint8_t *frame, size_t len, struct inkwell_esp_rom_response *out) {
    if (len < ESP_ROM_HEADER_LEN + ESP_ROM_STATUS_LEN || frame[0] != 0x01U) {
        return false;
    }
    const size_t size = (size_t)frame[2] | ((size_t)frame[3] << 8);
    if (ESP_ROM_HEADER_LEN + size != len || size < ESP_ROM_STATUS_LEN) {
        return false;
    }
    out->op = frame[1];
    out->value = get_u32(frame + 4);
    out->data = frame + ESP_ROM_HEADER_LEN;
    out->data_len = size;
    out->status = out->data[size - ESP_ROM_STATUS_LEN];
    out->error = out->data[size - ESP_ROM_STATUS_LEN + 1U];
    return true;
}

size_t inkwell_esp_rom_reader_feed(struct inkwell_esp_rom_reader *reader, const uint8_t *data,
                                   size_t len, struct inkwell_esp_rom_response *out, bool *ready) {
    if (ready != NULL) {
        *ready = false;
    }
    if (reader == NULL || data == NULL || out == NULL || ready == NULL) {
        return len;
    }
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        if (byte == SLIP_END) {
            if (reader->in_frame && reader->len > 0U && !reader->overflow &&
                parse(reader->frame, reader->len, out)) {
                reader->in_frame = false;
                reader->len = 0U;
                reader->escape = false;
                *ready = true;
                return i + 1U;
            }
            /* Not a response, so this delimiter opens one rather than closing one. */
            reader->in_frame = true;
            reader->len = 0U;
            reader->escape = false;
            reader->overflow = false;
            continue;
        }
        if (!reader->in_frame) {
            continue;
        }
        uint8_t value = byte;
        if (reader->escape) {
            reader->escape = false;
            if (byte == SLIP_ESC_END) {
                value = SLIP_END;
            } else if (byte == SLIP_ESC_ESC) {
                value = SLIP_ESC;
            } else {
                /* An escape of nothing: not SLIP, so not a frame. */
                reader->overflow = true;
                continue;
            }
        } else if (byte == SLIP_ESC) {
            reader->escape = true;
            continue;
        }
        if (reader->len >= sizeof reader->frame) {
            reader->overflow = true;
            continue;
        }
        reader->frame[reader->len++] = value;
    }
    return len;
}

bool inkwell_esp_rom_chip_for_magic(uint32_t magic, uint16_t *out_chip) {
    /* esptool's CHIP_DETECT_MAGIC_VALUE per chip. The C3 has had four silicon revisions,
       each with its own word. */
    static const struct {
        uint32_t magic;
        uint16_t chip;
    } k_magic[] = {
        {0x00F01D83U, INKWELL_ESP_CHIP_ESP32},    {0x000007C6U, INKWELL_ESP_CHIP_ESP32_S2},
        {0x00000009U, INKWELL_ESP_CHIP_ESP32_S3}, {0x6921506FU, INKWELL_ESP_CHIP_ESP32_C3},
        {0x1B31506FU, INKWELL_ESP_CHIP_ESP32_C3}, {0x4881606FU, INKWELL_ESP_CHIP_ESP32_C3},
        {0x4361606FU, INKWELL_ESP_CHIP_ESP32_C3}, {0x2CE0806FU, INKWELL_ESP_CHIP_ESP32_C6},
    };
    for (size_t i = 0; i < sizeof k_magic / sizeof k_magic[0]; ++i) {
        if (k_magic[i].magic == magic) {
            if (out_chip != NULL) {
                *out_chip = k_magic[i].chip;
            }
            return true;
        }
    }
    return false;
}

static int hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool inkwell_esp_rom_md5_matches(const struct inkwell_esp_rom_response *response,
                                 const uint8_t digest[16]) {
    if (response == NULL || digest == NULL || response->status != 0U ||
        response->op != INKWELL_ESP_ROM_SPI_FLASH_MD5 ||
        response->data_len < 32U + ESP_ROM_STATUS_LEN) {
        return false;
    }
    for (size_t i = 0; i < 16U; ++i) {
        const int high = hex_nibble(response->data[2U * i]);
        const int low = hex_nibble(response->data[2U * i + 1U]);
        if (high < 0 || low < 0 || (uint8_t)((high << 4) | low) != digest[i]) {
            return false;
        }
    }
    return true;
}

const char *inkwell_esp_rom_error_name(uint8_t error) {
    switch (error) {
    case 0x00U:
        return "none";
    case 0x05U:
        return "invalid message";
    case 0x06U:
        return "failed to act on message";
    case 0x07U:
        return "invalid checksum";
    case 0x08U:
        return "flash write error";
    case 0x09U:
        return "flash read error";
    case 0x0AU:
        return "flash read length error";
    case 0x0BU:
        return "deflate error";
    default:
        return "unknown error";
    }
}
