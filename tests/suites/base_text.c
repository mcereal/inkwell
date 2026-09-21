#define _POSIX_C_SOURCE 200809L

/* UTF-8: a character is one unit however many bytes it takes, and a malformed byte still
   advances the cursor. */

#include "framework/inkwell_test.h"

#include "inkwell/base/text.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Names and message bodies are the two places radio-chosen text reaches the screen, so the
 * UTF-8 helpers they both go through get pinned here: a character is one unit no matter how
 * many bytes it takes, and a malformed byte still advances the cursor.
 */
INKWELL_TEST_CASE(text_utf8_helpers, unit) {
    /* One four-byte emoji is one character. This is the bug the whole change is about: the
       framebuffer used to walk bytes, so a node named with a single emoji drew four cells. */
    const char *emoji = "\xF0\x9F\x93\xA1";
    INKWELL_TEST_FAIL_IF(inkwell_text_utf8_length(emoji) != 1U,
                         "a four-byte emoji should be one character");

    uint32_t codepoint = 0U;
    INKWELL_TEST_FAIL_IF(inkwell_text_utf8_next(emoji, &codepoint) != 4U || codepoint != 0x1F4E1U,
                         "emoji did not decode to U+1F4E1");

    struct {
        const char *label;
        const char *text;
        size_t chars;
    } lengths[] = {
        {"ascii", "Trail", 5U},
        {"accented", "Jos\xC3\xA9", 4U},
        {"mixed", "\xF0\x9F\x8C\xB2 Pine", 6U},
        {"empty", "", 0U},
        /* Each malformed byte counts as one character rather than stalling the walk. */
        {"malformed",
         "a\xFF\xFE"
         "b",
         4U},
    };
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_text_utf8_length(lengths[i].text) != lengths[i].chars,
                             lengths[i].label);
    }

    /* Truncation lands on a character boundary, never inside a sequence. */
    char line[32];
    snprintf(line, sizeof line, "%s", "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0\xF0\x9F\x9A\x97");
    inkwell_text_utf8_truncate(line, 2U);
    INKWELL_TEST_FAIL_IF(strcmp(line, "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0") != 0,
                         "truncate split a character");

    /* And a copy into a buffer too small for the next character stops before it, rather than
       leaving a half sequence behind. */
    char narrow[6];
    inkwell_text_sanitise_str("\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0", narrow, sizeof narrow);
    INKWELL_TEST_FAIL_IF(strcmp(narrow, "\xF0\x9F\x8C\xB2") != 0,
                         "sanitise split a character at the buffer boundary");

    record_success(test_name);
}
