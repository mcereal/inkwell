#pragma once

/*
 * A C struct to bytes and back, described by a table.
 *
 * Two programs that exchange records - over a socket, through a file - need the same three things
 * for every record: write it out field by field in a fixed byte order, read it back without
 * trusting a byte of it, and keep doing both while one side is a version newer than the other.
 * Written by hand per record, that is a pair of functions that drift apart the first time a field
 * is added to one and not the other. Here the record is described once, as rows:
 *
 *     struct reading { uint32_t id; bool on; char label[32]; };
 *     static const struct inkwell_field k_reading[] = {
 *         INKWELL_FIELD(U32, struct reading, id),
 *         INKWELL_FIELD(BOOL, struct reading, on),
 *         INKWELL_FIELD(STR, struct reading, label),
 *     };
 *
 * and one encoder and one decoder walk any table. A row's width is checked against its member's
 * when the table compiles: a U32 row over a uint16_t member is a build failure, not a record that
 * reads back wrong.
 *
 * **The wire.** Fields in table order, nothing between them, no names and no tags. Integers are
 * little-endian at their full width; a bool is one byte, 0 or 1. A string is a u16 byte count
 * and that many bytes of UTF-8, without the NUL. The table is the schema, and both ends hold it.
 *
 * **Changing a table is appending to it.** A decoder that runs out of bytes exactly between two
 * fields stops there and leaves the rest zero - that is a peer one version older, which did not
 * know the later fields - and bytes left over after the last field are ignored, which is a peer
 * one version newer. So a field may be added at the end and never anywhere else, and never
 * removed or retyped: a row that changes meaning in place is read by the old side as the old
 * meaning, silently. A field cut off half way is not an older peer; it is a broken message.
 *
 * **Decoding trusts nothing.** A bool that is not 0 or 1, a string that is not valid UTF-8 or
 * carries a NUL, and a string longer than its member can hold are all refused, and a refused
 * record is left zeroed rather than half-filled. The bytes come from another process, and the one
 * case worth guarding against is the one where that process is not the program it claims to be.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum inkwell_field_type {
    INKWELL_FIELD_BOOL = 1,
    INKWELL_FIELD_U8,
    INKWELL_FIELD_U16,
    INKWELL_FIELD_U32,
    INKWELL_FIELD_U64,
    INKWELL_FIELD_I32,
    INKWELL_FIELD_I64,
    /* A char array, NUL-terminated in memory. Its capacity is its size: at most size - 1 bytes
       of text go on the wire. */
    INKWELL_FIELD_STR,
};

/* One member of a record. Build rows with INKWELL_FIELD() rather than by hand. */
struct inkwell_field {
    enum inkwell_field_type type;
    uint16_t offset; /* offsetof() the member */
    uint16_t size;   /* sizeof() the member */
};

/* The width a fixed-size type has in memory and on the wire; 0 for a string, whose width is its
   member's. Used by INKWELL_FIELD() to check a row when the table compiles. */
#define INKWELL_FIELD_WIDTH_BOOL 1U
#define INKWELL_FIELD_WIDTH_U8 1U
#define INKWELL_FIELD_WIDTH_U16 2U
#define INKWELL_FIELD_WIDTH_U32 4U
#define INKWELL_FIELD_WIDTH_U64 8U
#define INKWELL_FIELD_WIDTH_I32 4U
#define INKWELL_FIELD_WIDTH_I64 8U
#define INKWELL_FIELD_WIDTH_STR 0U

/*
 * A row for `member` of `record_type`, read and written as `type_` (BOOL, U8, ... STR).
 *
 * The size is checked here: a fixed-width type over a member of another width - a U32 row on a
 * uint16_t - has a negative array size and does not compile. A string needs room for at least
 * one byte of text and its NUL, and no more than the row's u16 size can hold; a member that
 * starts past what the row's u16 offset can say does not compile either.
 */
#define INKWELL_FIELD(type_, record_type, member)                                                  \
    {INKWELL_FIELD_##type_, (uint16_t)offsetof(record_type, member),                               \
     (uint16_t)(sizeof(((record_type *)0)->member) +                                               \
                0U * sizeof(char[(offsetof(record_type, member) <= 65535U &&                       \
                                  (INKWELL_FIELD_WIDTH_##type_ == 0U                               \
                                       ? (sizeof(((record_type *)0)->member) >= 2U &&              \
                                          sizeof(((record_type *)0)->member) <= 65535U)            \
                                       : sizeof(((record_type *)0)->member) ==                     \
                                             INKWELL_FIELD_WIDTH_##type_))                         \
                                     ? 1                                                           \
                                     : -1]))}

/*
 * The most bytes a record of this table can encode to: every string full. What a caller sizes a
 * buffer by. Returns 0 for a table with a row it cannot encode.
 */
size_t inkwell_fields_encoded_max(const struct inkwell_field *fields, size_t count);

/*
 * Writes `record` into `out` in table order. Returns the number of bytes written; -EMSGSIZE when
 * `out` is too small (nothing useful is written), or -EINVAL for a table row that is not one
 * (an unknown type, a width that does not match) or a string member with no NUL inside it.
 */
int inkwell_fields_encode(const struct inkwell_field *fields, size_t count, const void *record,
                          uint8_t *out, size_t out_len);

/*
 * Reads `len` bytes into `record`. Every member the table names is zeroed first; bytes of the
 * struct past the last of them are not touched, so a larger, newer struct can be read through an
 * older table.
 *
 * Returns how many of the table's fields the bytes carried - `count` from a peer of the same
 * version, fewer from an older one (the rest stay zero). Bytes after the last field are ignored.
 * Returns -EBADMSG for a field cut off part way, a bool that is not 0 or 1, or a string that is
 * not valid UTF-8 or contains a NUL; -EMSGSIZE for a string longer than its member holds;
 * -EINVAL for a table row that is not one. On any error `record` is left zeroed.
 */
int inkwell_fields_decode(const struct inkwell_field *fields, size_t count, const uint8_t *in,
                          size_t len, void *record);

#ifdef __cplusplus
}
#endif
