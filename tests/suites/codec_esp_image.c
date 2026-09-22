#include "framework/inkwell_test.h"
#include "inkwell/codec/esp_image.h"

#include <string.h>

/* First 48 bytes from firmware-heltec-v3-2.7.26.54e0d8d.bin. */
static const uint8_t k_esp_header[48] = {
    0xE9, 0x07, 0x02, 0x3F, 0xD8, 0x72, 0x37, 0x40, 0xEE, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x18, 0x3C, 0x30, 0x37, 0x07, 0x00,
    0x32, 0x54, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

INKWELL_TEST_CASE(esp_image_reads_a_real_header, unit) {
    /* The header is the release's own bytes; see the fixture. */
    uint8_t image[64];
    memset(image, 0, sizeof image);
    memcpy(image, k_esp_header, sizeof k_esp_header);
    struct inkwell_esp_image_info info;
    INKWELL_TEST_FAIL_IF(
        inkwell_esp_image_validate(image, sizeof image, INKWELL_ESP_CHIP_ESP32_S3, &info) !=
                INKWELL_ESP_IMAGE_OK ||
            info.chip_id != INKWELL_ESP_CHIP_ESP32_S3 || info.segments != 7U,
        "the Heltec V3's 2.7.26 image is an ESP32-S3 application of seven segments");
    INKWELL_TEST_FAIL_IF(inkwell_esp_image_validate(image, sizeof image, INKWELL_ESP_CHIP_ESP32,
                                                    &info) != INKWELL_ESP_IMAGE_WRONG_CHIP ||
                             info.chip_id != INKWELL_ESP_CHIP_ESP32_S3,
                         "and not an ESP32 one - chip 0 is a real chip, not a wildcard - and a "
                         "refusal still says which chip it was for");
    INKWELL_TEST_FAIL_IF(inkwell_esp_image_validate(image, INKWELL_ESP_IMAGE_MIN_LEN - 1U,
                                                    INKWELL_ESP_CHIP_ESP32_S3,
                                                    NULL) != INKWELL_ESP_IMAGE_TOO_SHORT,
                         "a header cut short is too short");
    image[32] ^= 0xFFU;
    INKWELL_TEST_FAIL_IF(inkwell_esp_image_validate(image, sizeof image, INKWELL_ESP_CHIP_ESP32_S3,
                                                    NULL) != INKWELL_ESP_IMAGE_NOT_AN_APP,
                         "no app descriptor is not an application: a bootloader, a factory image");
    image[32] ^= 0xFFU;
    image[0] = 0xE8U;
    INKWELL_TEST_FAIL_IF(inkwell_esp_image_validate(image, sizeof image, INKWELL_ESP_CHIP_ESP32_S3,
                                                    NULL) != INKWELL_ESP_IMAGE_BAD_MAGIC,
                         "no 0xE9 is not an ESP32 image at all");

    record_success(test_name);
}
