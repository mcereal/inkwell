/* Zeroing a buffer that held a secret. What the compiler may not elide cannot be observed from
   a test that reads the buffer back; this holds what it can: every byte, and nothing else. */

#include "framework/inkwell_test.h"

#include "inkwell/base/wipe.h"

#include <stdint.h>
#include <string.h>

INKWELL_TEST_CASE(wipe_zeroes_exactly_what_it_is_given, unit) {
    uint8_t buffer[16];
    memset(buffer, 0xA5, sizeof buffer);
    inkwell_wipe(buffer + 4, 8U);
    for (size_t i = 0; i < sizeof buffer; ++i) {
        const uint8_t want = (i >= 4U && i < 12U) ? 0U : 0xA5U;
        INKWELL_TEST_FAIL_IF(buffer[i] != want, "the range is zeroed and nothing outside it");
    }
    inkwell_wipe(NULL, 8U);
    inkwell_wipe(buffer, 0U);
    record_success(test_name);
}
