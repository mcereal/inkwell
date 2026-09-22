#pragma once

/*
 * Reading the header of an ESP32 application image: the `.bin` handed to a loader.
 *
 * The loader checks bytes against a supplied SHA-256, but a digest computed over the same
 * file does not establish that the image was built for this chip. The header can reject a
 * mismatched image before a potentially long transfer.
 *
 * What it can answer, and only that: the magic, the chip the image was built for, and whether
 * the first segment carries an `esp_app_desc_t` (which every ESP-IDF application does, and a
 * bootloader or a partition table does not). It does **not** say which *board* the image is for
 * - two distinct boards may use the same chip. A caller checks board identity using its
 * own image metadata.
 *
 * Measured against `firmware-heltec-v3-2.7.26.54e0d8d.bin` on 2026-09-11: magic `0xE9`, seven
 * segments, chip id `0x0009`, and the app descriptor's magic word at offset 32. Its `version`
 * field says `esp-idf: v4.4.7 38eeba213a` and its project name `arduino-lib-builder`, so neither
 * identifies the firmware release and neither is checked.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `esp_chip_id_t`, as ESP-IDF numbers them. 0 is a real chip here - the original ESP32 - so
   "no chip" must be represented separately rather than with zero. */
#define INKWELL_ESP_CHIP_ESP32 0x0000U
#define INKWELL_ESP_CHIP_ESP32_S2 0x0002U
#define INKWELL_ESP_CHIP_ESP32_C3 0x0005U
#define INKWELL_ESP_CHIP_ESP32_S3 0x0009U
#define INKWELL_ESP_CHIP_ESP32_C6 0x000DU

/* The fixed header plus the first segment's header plus the app descriptor's magic word. */
#define INKWELL_ESP_IMAGE_MIN_LEN 36U

enum inkwell_esp_image_verdict {
    INKWELL_ESP_IMAGE_OK = 0,
    INKWELL_ESP_IMAGE_TOO_SHORT,
    /* The first byte is not 0xE9: not an ESP32 image at all. */
    INKWELL_ESP_IMAGE_BAD_MAGIC,
    /* An image, for another chip. The one this check is for. */
    INKWELL_ESP_IMAGE_WRONG_CHIP,
    /* An image for this chip with no app descriptor where an application keeps one: a
       bootloader, or the `.factory.bin`, which starts with the bootloader. */
    INKWELL_ESP_IMAGE_NOT_AN_APP,
};

struct inkwell_esp_image_info {
    uint16_t chip_id;
    uint8_t segments;
};

/*
 * Checks `bytes` is an ESP32 application image built for `expect_chip`.
 *
 * `info` may be NULL, and is filled in as far as the header could be read even on a refusal, so
 * the log can say which chip the wrong image was for.
 */
enum inkwell_esp_image_verdict inkwell_esp_image_validate(const uint8_t *bytes, size_t len,
                                                          uint16_t expect_chip,
                                                          struct inkwell_esp_image_info *info);

const char *inkwell_esp_image_verdict_name(enum inkwell_esp_image_verdict verdict);

#ifdef __cplusplus
}
#endif
