/* The ESP32 ROM bootloader's serial protocol, against bytes captured from a Heltec V3
   (ESP32-S3) in download mode. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/esp_image.h"
#include "inkwell/codec/esp_rom.h"

#include <errno.h>
#include <string.h>

/* Decodes one framed request back to its bytes, so a test reads what was said. */
static size_t unslip(const uint8_t *in, size_t len, uint8_t *out) {
    size_t at = 0U;
    for (size_t i = 1U; i + 1U < len; ++i) {
        if (in[i] == 0xDBU) {
            out[at++] = in[i + 1U] == 0xDCU ? 0xC0U : 0xDBU;
            ++i;
        } else {
            out[at++] = in[i];
        }
    }
    return at;
}

INKWELL_TEST_CASE(esp_rom_sync_is_the_bytes_the_rom_tunes_on, unit) {
    uint8_t out[INKWELL_ESP_ROM_REQUEST_MAX];
    const int len = inkwell_esp_rom_sync(out, sizeof out);
    uint8_t expected[46] = {0xC0, 0x00, 0x08, 0x24, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x07, 0x07, 0x12, 0x20};
    memset(expected + 13, 0x55, 32);
    expected[45] = 0xC0;
    INKWELL_TEST_FAIL_IF(len != (int)sizeof expected || memcmp(out, expected, sizeof expected) != 0,
                         "SYNC is 36 bytes: 07 07 12 20 and thirty-two 0x55");
    INKWELL_TEST_FAIL_IF(inkwell_esp_rom_sync(out, 20U) != -ENOBUFS, "a short buffer refuses");
}

INKWELL_TEST_CASE(esp_rom_flash_begin_counts_words_by_chip, unit) {
    uint8_t out[INKWELL_ESP_ROM_REQUEST_MAX];
    uint8_t raw[64];
    const int s3 =
        inkwell_esp_rom_flash_begin(INKWELL_ESP_CHIP_ESP32_S3, 644000U, 0x10000U, out, sizeof out);
    const size_t s3_len = unslip(out, (size_t)s3, raw);
    INKWELL_TEST_FAIL_IF(s3_len != 8U + 20U || raw[2] != 20U,
                         "every ROM after the ESP32's takes five words");
    /* 644000 bytes is 629 blocks of 1 KB, the last one short. */
    INKWELL_TEST_FAIL_IF(raw[12] != 0x75U || raw[13] != 0x02U || raw[16] != 0x00U ||
                             raw[17] != 0x04U || raw[22] != 0x01U,
                         "size, blocks, block size, offset");
    const int esp32 =
        inkwell_esp_rom_flash_begin(INKWELL_ESP_CHIP_ESP32, 644000U, 0x10000U, out, sizeof out);
    INKWELL_TEST_FAIL_IF(unslip(out, (size_t)esp32, raw) != 8U + 16U,
                         "the original ESP32's takes four and refuses a fifth");
    INKWELL_TEST_FAIL_IF(inkwell_esp_rom_blocks(1024U) != 1U || inkwell_esp_rom_blocks(1025U) != 2U,
                         "blocks round up");
}

INKWELL_TEST_CASE(esp_rom_flash_data_pads_escapes_and_checksums, unit) {
    uint8_t block[3] = {0xC0U, 0xDBU, 0x01U};
    uint8_t out[INKWELL_ESP_ROM_REQUEST_MAX];
    uint8_t raw[INKWELL_ESP_ROM_REQUEST_MAX];
    const int len = inkwell_esp_rom_flash_data(7U, block, sizeof block, out, sizeof out);
    INKWELL_TEST_FAIL_IF(len <= 0, "a short block is a block");
    for (int i = 1; i + 1 < len; ++i) {
        INKWELL_TEST_FAIL_IF(out[i] == 0xC0U, "no delimiter inside a frame");
    }
    const size_t raw_len = unslip(out, (size_t)len, raw);
    INKWELL_TEST_FAIL_IF(raw_len != 8U + 16U + INKWELL_ESP_ROM_BLOCK,
                         "padded to a whole block with its sixteen bytes of header");
    INKWELL_TEST_FAIL_IF(raw[12] != 7U, "the sequence number");
    INKWELL_TEST_FAIL_IF(raw[24] != 0xC0U || raw[25] != 0xDBU || raw[26] != 0x01U ||
                             raw[27] != 0xFFU || raw[raw_len - 1U] != 0xFFU,
                         "the bytes, then erased flash");
    /* 0xEF ^ C0 ^ DB ^ 01, and 1021 0xFFs, an odd count, flip it once more. */
    const uint8_t checksum = (uint8_t)(0xEFU ^ 0xC0U ^ 0xDBU ^ 0x01U ^ 0xFFU);
    INKWELL_TEST_FAIL_IF(raw[4] != checksum || raw[5] != 0U, "0xEF XORed over the padded block");
    INKWELL_TEST_FAIL_IF(inkwell_esp_rom_flash_data(0U, block, INKWELL_ESP_ROM_BLOCK + 1U, out,
                                                    sizeof out) != -EINVAL,
                         "more than a block refuses");
}

INKWELL_TEST_CASE(esp_rom_reader_finds_answers_in_a_noisy_port, unit) {
    /* The boot banner a V3 prints on its way into download mode, then the answer to SYNC it
       gave - split across two reads, the way a port delivers it. */
    static const char k_banner[] = "ESP-ROM:esp32s3-20210327\r\nwaiting for download\r\n";
    static const uint8_t k_sync_answer[] = {0xC0, 0x01, 0x08, 0x04, 0x00, 0x07, 0x07,
                                            0x12, 0x20, 0x00, 0x00, 0x00, 0x00, 0xC0};
    struct inkwell_esp_rom_reader reader;
    inkwell_esp_rom_reader_reset(&reader);
    struct inkwell_esp_rom_response response;
    bool ready = false;
    size_t used = inkwell_esp_rom_reader_feed(&reader, (const uint8_t *)k_banner,
                                              sizeof k_banner - 1U, &response, &ready);
    INKWELL_TEST_FAIL_IF(ready || used != sizeof k_banner - 1U, "text is not an answer");
    used = inkwell_esp_rom_reader_feed(&reader, k_sync_answer, 6U, &response, &ready);
    INKWELL_TEST_FAIL_IF(ready, "half an answer is not one");
    used = inkwell_esp_rom_reader_feed(&reader, k_sync_answer + 6, sizeof k_sync_answer - 6U,
                                       &response, &ready);
    INKWELL_TEST_FAIL_IF(!ready || used != sizeof k_sync_answer - 6U, "the rest completes it");
    INKWELL_TEST_FAIL_IF(response.op != INKWELL_ESP_ROM_SYNC || response.value != 0x20120707U ||
                             response.status != 0U || response.data_len != 4U,
                         "SYNC's answer: its op, the ROM's word, success");

    /* Joined mid-frame: the tail of one answer, then a whole one. The whole one is found. */
    static const uint8_t k_joined[] = {0x00, 0x00, 0xC0, 0xC0, 0x01, 0x0A, 0x04, 0x00, 0x09,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0};
    inkwell_esp_rom_reader_reset(&reader);
    used = inkwell_esp_rom_reader_feed(&reader, k_joined, sizeof k_joined, &response, &ready);
    uint16_t chip = 0xFFFFU;
    INKWELL_TEST_FAIL_IF(!ready || response.op != INKWELL_ESP_ROM_READ_REG ||
                             !inkwell_esp_rom_chip_for_magic(response.value, &chip) ||
                             chip != INKWELL_ESP_CHIP_ESP32_S3,
                         "the V3 reads its magic word as 9: an ESP32-S3");

    /* Two answers in one read: the first is returned, the rest is fed again. */
    uint8_t two[2U * sizeof k_sync_answer];
    memcpy(two, k_sync_answer, sizeof k_sync_answer);
    memcpy(two + sizeof k_sync_answer, k_sync_answer, sizeof k_sync_answer);
    inkwell_esp_rom_reader_reset(&reader);
    used = inkwell_esp_rom_reader_feed(&reader, two, sizeof two, &response, &ready);
    INKWELL_TEST_FAIL_IF(!ready || used != sizeof k_sync_answer, "stops after the first");
    used = inkwell_esp_rom_reader_feed(&reader, two + used, sizeof two - used, &response, &ready);
    INKWELL_TEST_FAIL_IF(!ready, "and the second is still there");
}

INKWELL_TEST_CASE(esp_rom_reads_a_refusal_and_an_md5, unit) {
    /* What the V3 said to a 16 KB FLASH_BEGIN: status 1, error 5. */
    static const uint8_t k_refused[] = {0xC0, 0x01, 0x02, 0x04, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0xC0};
    struct inkwell_esp_rom_reader reader;
    inkwell_esp_rom_reader_reset(&reader);
    struct inkwell_esp_rom_response response;
    bool ready = false;
    (void)inkwell_esp_rom_reader_feed(&reader, k_refused, sizeof k_refused, &response, &ready);
    INKWELL_TEST_FAIL_IF(
        !ready || response.status != 1U || response.error != 5U ||
            strcmp(inkwell_esp_rom_error_name(response.error), "invalid message") != 0,
        "a refusal carries its error");

    /* The V3's answer to SPI_FLASH_MD5 over MeshCore 1.17.1's app: hex text, then status. */
    static const char k_hex[] = "e3ed7342fc711f2fc28dcfa89c953e33";
    uint8_t frame[64] = {0xC0, 0x01, 0x13, 36, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(frame + 9, k_hex, 32);
    memset(frame + 41, 0, 4);
    frame[45] = 0xC0;
    static const uint8_t k_digest[16] = {0xe3, 0xed, 0x73, 0x42, 0xfc, 0x71, 0x1f, 0x2f,
                                         0xc2, 0x8d, 0xcf, 0xa8, 0x9c, 0x95, 0x3e, 0x33};
    inkwell_esp_rom_reader_reset(&reader);
    (void)inkwell_esp_rom_reader_feed(&reader, frame, 46U, &response, &ready);
    INKWELL_TEST_FAIL_IF(!ready || !inkwell_esp_rom_md5_matches(&response, k_digest),
                         "the flash holds what was sent");
    uint8_t other[16];
    memcpy(other, k_digest, sizeof other);
    other[15] ^= 1U;
    INKWELL_TEST_FAIL_IF(inkwell_esp_rom_md5_matches(&response, other), "and nothing else");
    INKWELL_TEST_FAIL_IF(inkwell_esp_rom_chip_for_magic(0x12345678U, NULL),
                         "an unknown magic is no chip");
}
