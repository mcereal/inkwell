#include "inkwell/codec/esp_image.h"

#include <string.h>

/* esp_image_header_t: magic, segment count, two flash bytes, entry point, then the pin fields
   and the chip id at offset 12 - little-endian, like everything the chip writes. */
#define ESP_IMAGE_MAGIC 0xE9U
#define ESP_IMAGE_SEGMENTS_OFFSET 1U
#define ESP_IMAGE_CHIP_OFFSET 12U
/* The fixed header is 24 bytes and the first segment's header 8 more, so an application's
   esp_app_desc_t - which ESP-IDF places at the very start of the first segment - begins at 32. */
#define ESP_FIRST_SEGMENT_LEN_OFFSET 28U
#define ESP_APP_DESC_OFFSET 32U
#define ESP_APP_DESC_MAGIC 0xABCD5432UL

static const char *const k_verdict_names[] = {
    "ok", "too short", "bad magic", "wrong chip", "not an application",
};

const char *inkwell_esp_image_verdict_name(enum inkwell_esp_image_verdict verdict) {
    return (size_t)verdict < sizeof k_verdict_names / sizeof k_verdict_names[0]
               ? k_verdict_names[verdict]
               : "?";
}

enum inkwell_esp_image_verdict inkwell_esp_image_validate(const uint8_t *bytes, size_t len,
                                                          uint16_t expect_chip,
                                                          struct inkwell_esp_image_info *info) {
    struct inkwell_esp_image_info scratch;
    struct inkwell_esp_image_info *const out = info != NULL ? info : &scratch;
    memset(out, 0, sizeof *out);

    if (bytes == NULL || len < INKWELL_ESP_IMAGE_MIN_LEN) {
        return INKWELL_ESP_IMAGE_TOO_SHORT;
    }
    if (bytes[0] != ESP_IMAGE_MAGIC) {
        return INKWELL_ESP_IMAGE_BAD_MAGIC;
    }
    out->segments = bytes[ESP_IMAGE_SEGMENTS_OFFSET];
    out->chip_id =
        (uint16_t)(bytes[ESP_IMAGE_CHIP_OFFSET] | (uint16_t)bytes[ESP_IMAGE_CHIP_OFFSET + 1U] << 8);
    if (out->chip_id != expect_chip) {
        return INKWELL_ESP_IMAGE_WRONG_CHIP;
    }
    const uint32_t first_segment_len = (uint32_t)bytes[ESP_FIRST_SEGMENT_LEN_OFFSET] |
                                       (uint32_t)bytes[ESP_FIRST_SEGMENT_LEN_OFFSET + 1U] << 8 |
                                       (uint32_t)bytes[ESP_FIRST_SEGMENT_LEN_OFFSET + 2U] << 16 |
                                       (uint32_t)bytes[ESP_FIRST_SEGMENT_LEN_OFFSET + 3U] << 24;
    if (out->segments == 0U || first_segment_len < sizeof(uint32_t)) {
        return INKWELL_ESP_IMAGE_NOT_AN_APP;
    }
    const uint32_t desc_magic = (uint32_t)bytes[ESP_APP_DESC_OFFSET] |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 1U] << 8 |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 2U] << 16 |
                                (uint32_t)bytes[ESP_APP_DESC_OFFSET + 3U] << 24;
    if (desc_magic != ESP_APP_DESC_MAGIC) {
        return INKWELL_ESP_IMAGE_NOT_AN_APP;
    }
    return INKWELL_ESP_IMAGE_OK;
}
