#define _POSIX_C_SOURCE 200809L

/* A struct to bytes and back through a table: the wire, the version rules, and the refusals. */

#include "framework/inkwell_test.h"

#include "inkwell/codec/fields.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct sample {
    uint32_t id;
    bool on;
    int32_t delta;
    char label[8];
    uint64_t stamp;
};

static const struct inkwell_field k_sample[] = {
    INKWELL_FIELD(U32, struct sample, id),    INKWELL_FIELD(BOOL, struct sample, on),
    INKWELL_FIELD(I32, struct sample, delta), INKWELL_FIELD(STR, struct sample, label),
    INKWELL_FIELD(U64, struct sample, stamp),
};
#define SAMPLE_FIELDS (sizeof k_sample / sizeof k_sample[0])

INKWELL_TEST_CASE(fields_round_trip_and_spell_the_wire, unit) {
    const struct sample in = {
        .id = 0x01020304U, .on = true, .delta = -2, .label = "hi", .stamp = 0x1122334455667788ULL};
    uint8_t wire[64];
    const int n = inkwell_fields_encode(k_sample, SAMPLE_FIELDS, &in, wire, sizeof wire);
    const uint8_t want[] = {0x04, 0x03, 0x02, 0x01,                          /* id, little-endian */
                            0x01,                                            /* on */
                            0xFE, 0xFF, 0xFF, 0xFF,                          /* -2 */
                            0x02, 0x00, 'h',  'i',                           /* count, no NUL */
                            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}; /* stamp */
    INKWELL_TEST_FAIL_IF(n != (int)sizeof want || memcmp(wire, want, sizeof want) != 0,
                         "the wire is fields in order, little-endian, strings counted");

    struct sample out;
    memset(&out, 0xAA, sizeof out);
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, wire, (size_t)n, &out) !=
                             (int)SAMPLE_FIELDS,
                         "a whole record reads every field");
    INKWELL_TEST_FAIL_IF(out.id != in.id || !out.on || out.delta != -2 ||
                             strcmp(out.label, "hi") != 0 || out.stamp != in.stamp,
                         "what was written reads back");
    INKWELL_TEST_FAIL_IF(out.label[3] != '\0', "the string's tail is zeroed, not left as it was");
    INKWELL_TEST_FAIL_IF(inkwell_fields_encoded_max(k_sample, SAMPLE_FIELDS) !=
                             4U + 1U + 4U + 2U + 7U + 8U,
                         "the maximum is every string full");
    record_success(test_name);
}

INKWELL_TEST_CASE(fields_an_older_or_newer_peer_still_reads, unit) {
    const struct sample in = {.id = 7U, .on = true, .delta = 3, .label = "abc", .stamp = 9U};
    uint8_t wire[64];
    const int n = inkwell_fields_encode(k_sample, SAMPLE_FIELDS, &in, wire, sizeof wire);
    INKWELL_TEST_FAIL_IF(n <= 0, "the record encodes");

    /* An older peer knew only the first two fields: it sent five bytes, and the rest read zero. */
    struct sample out;
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, wire, 5U, &out) != 2,
                         "an older peer's record reads the fields it had");
    INKWELL_TEST_FAIL_IF(out.id != 7U || !out.on || out.delta != 0 || out.label[0] != '\0',
                         "and the fields it did not know are zero");

    /* A newer peer appended a field: an older table reads its own and ignores the tail. */
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, 2U, wire, (size_t)n, &out) != 2 ||
                             out.id != 7U || !out.on,
                         "a newer peer's extra fields are ignored");

    /* Cut inside a field is not an older peer, it is a broken message - and it leaves nothing. */
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, wire, 3U, &out) != -EBADMSG,
                         "a field cut part way is refused");
    INKWELL_TEST_FAIL_IF(out.id != 0U, "a refused record is left zeroed");
    record_success(test_name);
}

INKWELL_TEST_CASE(fields_decoding_trusts_nothing, unit) {
    struct sample out;
    /* id, then a bool of 2 */
    const uint8_t bad_bool[] = {1, 0, 0, 0, 2};
    INKWELL_TEST_FAIL_IF(
        inkwell_fields_decode(k_sample, SAMPLE_FIELDS, bad_bool, sizeof bad_bool, &out) != -EBADMSG,
        "a bool is 0 or 1");

    /* id, on, delta, then a string of 8 bytes into a member that holds 7 */
    uint8_t long_str[] = {1, 0, 0, 0, 1, 0, 0, 0, 0, 8, 0, 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'};
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, long_str, sizeof long_str,
                                               &out) != -EMSGSIZE,
                         "a string longer than its member is refused, not cut");

    /* the same string with a NUL inside it, and then with a lone continuation byte */
    long_str[9] = 3;
    long_str[12] = 0;
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, long_str, 14U, &out) !=
                             -EBADMSG,
                         "a NUL inside a string is refused");
    long_str[12] = 0x80;
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, long_str, 14U, &out) !=
                             -EBADMSG,
                         "a string that is not UTF-8 is refused");

    /* a count that runs past the end of the message */
    long_str[9] = 200;
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_sample, SAMPLE_FIELDS, long_str, 14U, &out) !=
                             -EBADMSG,
                         "a string count past the end is refused");
    record_success(test_name);
}

INKWELL_TEST_CASE(fields_encoding_refuses_rather_than_cuts, unit) {
    const struct sample in = {.id = 1U, .label = "abcdef"};
    uint8_t wire[64];
    INKWELL_TEST_FAIL_IF(inkwell_fields_encode(k_sample, SAMPLE_FIELDS, &in, wire, 10U) !=
                             -EMSGSIZE,
                         "a short buffer is refused");

    struct sample unterminated = in;
    memset(unterminated.label, 'x', sizeof unterminated.label);
    INKWELL_TEST_FAIL_IF(
        inkwell_fields_encode(k_sample, SAMPLE_FIELDS, &unterminated, wire, sizeof wire) != -EINVAL,
        "a member with no NUL is not a string");

    /* A table written by hand gets the check INKWELL_FIELD() does at compile time. */
    const struct inkwell_field wrong[] = {{INKWELL_FIELD_U32, 0U, 2U}};
    INKWELL_TEST_FAIL_IF(inkwell_fields_encode(wrong, 1U, &in, wire, sizeof wire) != -EINVAL,
                         "a row whose width does not match its type is refused");
    INKWELL_TEST_FAIL_IF(inkwell_fields_encoded_max(wrong, 1U) != 0U, "and has no maximum");
    record_success(test_name);
}

INKWELL_TEST_CASE(fields_decoding_touches_only_what_the_table_names, unit) {
    /* A member between two rows that never goes on the wire keeps what the caller put there. */
    struct mixed {
        uint32_t id;
        uint32_t local;
        uint16_t port;
    };
    static const struct inkwell_field k_mixed[] = {
        INKWELL_FIELD(U32, struct mixed, id),
        INKWELL_FIELD(U16, struct mixed, port),
    };
    const uint8_t wire[] = {5, 0, 0, 0, 9, 0};
    struct mixed out = {.local = 77U};
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(k_mixed, 2U, wire, sizeof wire, &out) != 2 ||
                             out.id != 5U || out.port != 9U || out.local != 77U,
                         "a member the table does not name is not zeroed");

    /* A bad row is refused however short the message is that would have reached it. */
    const struct inkwell_field bad_tail[] = {INKWELL_FIELD(U32, struct mixed, id),
                                             {INKWELL_FIELD_U32, 4U, 2U}};
    INKWELL_TEST_FAIL_IF(inkwell_fields_decode(bad_tail, 2U, wire, 4U, &out) != -EINVAL,
                         "a table with a bad row is refused before any field is read");
    record_success(test_name);
}
