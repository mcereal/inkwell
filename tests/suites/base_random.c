/* The system's random bytes. Randomness cannot be proved from a sample; what can be held is that
   every byte is written, that two draws differ, and that nothing is asked for nothing. */

#include "framework/inkwell_test.h"

#include "inkwell/base/random.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

INKWELL_TEST_CASE(random_fills_every_byte_and_never_repeats, unit) {
    uint8_t a[300];
    uint8_t b[300];
    INKWELL_TEST_FAIL_IF(inkwell_random_bytes(a, sizeof a) != 0 ||
                             inkwell_random_bytes(b, sizeof b) != 0,
                         "two draws, the first longer than one getrandom() promises");
    INKWELL_TEST_FAIL_IF(memcmp(a, b, sizeof a) == 0, "two draws are not the same bytes");
    size_t zeros = 0U;
    for (size_t i = 0U; i < sizeof a; ++i) {
        zeros += a[i] == 0U ? 1U : 0U;
    }
    INKWELL_TEST_FAIL_IF(zeros > 20U, "a draw is not mostly zeros: every byte was written");
    INKWELL_TEST_FAIL_IF(inkwell_random_bytes(NULL, 0U) != 0, "nothing asked: nothing to do");
    INKWELL_TEST_FAIL_IF(inkwell_random_bytes(NULL, 4U) != -EINVAL, "bytes into nowhere");
    record_success(test_name);
}
