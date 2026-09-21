#define _POSIX_C_SOURCE 200809L

/* Base64 in both alphabets, and the two strictnesses about padding. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/base64.h"

#include <stdint.h>
#include <string.h>

INKWELL_TEST_CASE(base64_round_trips_both_alphabets, unit) {
    /* The two characters that differ, in a payload chosen to produce both of them: 0x3E and
       0x3F are '+' and '/' in one alphabet and '-' and '_' in the other. */
    const uint8_t bytes[] = {0xFBU, 0xF0U, 0x00U, 0x01U, 0x02U};
    char standard[32];
    char url[32];
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_encode(bytes, sizeof bytes, false, standard, sizeof standard) == 0U,
        "the standard encoding did not fit");
    INKWELL_TEST_FAIL_IF(inkwell_base64_encode(bytes, sizeof bytes, true, url, sizeof url) == 0U,
                         "the URL-safe encoding did not fit");
    INKWELL_TEST_FAIL_IF(strcmp(standard, "+/AAAQI=") != 0, "the standard encoding came out wrong");
    INKWELL_TEST_FAIL_IF(strcmp(url, "-_AAAQI") != 0, "the URL-safe encoding came out wrong");

    /* Either spelling decodes, whichever was asked for, and padding is optional in ANY. */
    uint8_t out[8];
    size_t len = 0U;
    INKWELL_TEST_FAIL_IF(!inkwell_base64_decode(standard, strlen(standard), INKWELL_BASE64_ANY, out,
                                                sizeof out, &len) ||
                             len != sizeof bytes || memcmp(out, bytes, len) != 0,
                         "the standard encoding did not decode back");
    INKWELL_TEST_FAIL_IF(
        !inkwell_base64_decode(url, strlen(url), INKWELL_BASE64_ANY, out, sizeof out, &len) ||
            len != sizeof bytes || memcmp(out, bytes, len) != 0,
        "the URL-safe encoding did not decode back");

    /* An output buffer one byte short refuses rather than writing what fits. */
    INKWELL_TEST_FAIL_IF(inkwell_base64_encode(bytes, sizeof bytes, false, standard, 8U) != 0U,
                         "a short buffer was written to anyway");
    INKWELL_TEST_FAIL_IF(standard[0] != '\0', "a refused encoding left text behind");
    record_success(test_name);
}

/*
 * The strictness is the point of the parameter, and the case that says so is a *key*: it is a
 * fixed number of bytes, a mistyped one decodes to a plausible wrong key, and nothing on the
 * wire reports it - the radio just stops hearing the mesh.
 */
INKWELL_TEST_CASE(base64_padding_strictness_differs_by_form, unit) {
    uint8_t out[8];
    size_t len = 0U;

    /* Three characters: two bytes' worth, and a group the padded form will not take. */
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_decode("abc", 3U, INKWELL_BASE64_PADDED, out, sizeof out, &len),
        "the padded form took a short final group");
    INKWELL_TEST_FAIL_IF(
        !inkwell_base64_decode("abc", 3U, INKWELL_BASE64_ANY, out, sizeof out, &len) || len != 2U,
        "the forgiving form refused a short final group");

    /* One '=' where two belong is a character lost in transit, not a short string. */
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_decode("AQ=", 3U, INKWELL_BASE64_PADDED, out, sizeof out, &len),
        "the padded form took a half-padded group");

    /* A group of one character carries no whole byte: neither form takes it. */
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_decode("AQIDB", 5U, INKWELL_BASE64_ANY, out, sizeof out, &len),
        "a trailing single character was accepted");

    /* Padding anywhere but the tail is refused by both, and so is anything off the alphabet. */
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_decode("A=QI", 4U, INKWELL_BASE64_ANY, out, sizeof out, &len),
        "padding in the middle was accepted");
    INKWELL_TEST_FAIL_IF(
        inkwell_base64_decode("AQ I", 4U, INKWELL_BASE64_ANY, out, sizeof out, &len),
        "a space was accepted");
    record_success(test_name);
}
