#include "inkwell/codec/fields.h"

#include "inkwell/base/text.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* The wire width of a fixed-size type, 0 for a string, or SIZE_MAX for a type that is not one. */
static size_t field_width(enum inkwell_field_type type) {
    switch (type) {
    case INKWELL_FIELD_BOOL:
    case INKWELL_FIELD_U8:
        return 1U;
    case INKWELL_FIELD_U16:
        return 2U;
    case INKWELL_FIELD_U32:
    case INKWELL_FIELD_I32:
        return 4U;
    case INKWELL_FIELD_U64:
    case INKWELL_FIELD_I64:
        return 8U;
    case INKWELL_FIELD_STR:
        return 0U;
    }
    return SIZE_MAX;
}

/* A row INKWELL_FIELD() would have built: a known type, and a size that fits it. A table built
   by hand gets the same check at run time. */
static bool field_valid(const struct inkwell_field *field) {
    const size_t width = field_width(field->type);
    if (width == SIZE_MAX) {
        return false;
    }
    return width == 0U ? field->size >= 2U : field->size == width;
}

static void put_le(uint8_t *out, uint64_t value, size_t width) {
    for (size_t i = 0U; i < width; ++i) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

static uint64_t get_le(const uint8_t *in, size_t width) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; ++i) {
        value |= (uint64_t)in[i] << (8U * i);
    }
    return value;
}

/* The member's value widened to 64 bits, read through its real type so a signed one keeps its
   sign and nothing depends on the host's byte order. */
static uint64_t load(const uint8_t *member, enum inkwell_field_type type) {
    switch (type) {
    case INKWELL_FIELD_BOOL: {
        bool v;
        memcpy(&v, member, sizeof v);
        return v ? 1U : 0U;
    }
    case INKWELL_FIELD_U8:
        return member[0];
    case INKWELL_FIELD_U16: {
        uint16_t v;
        memcpy(&v, member, sizeof v);
        return v;
    }
    case INKWELL_FIELD_U32: {
        uint32_t v;
        memcpy(&v, member, sizeof v);
        return v;
    }
    case INKWELL_FIELD_I32: {
        int32_t v;
        memcpy(&v, member, sizeof v);
        return (uint64_t)(uint32_t)v;
    }
    case INKWELL_FIELD_U64:
    case INKWELL_FIELD_I64: {
        uint64_t v;
        memcpy(&v, member, sizeof v);
        return v;
    }
    case INKWELL_FIELD_STR:
        break;
    }
    return 0U;
}

static void store(uint8_t *member, enum inkwell_field_type type, uint64_t value) {
    switch (type) {
    case INKWELL_FIELD_BOOL: {
        const bool v = value != 0U;
        memcpy(member, &v, sizeof v);
        return;
    }
    case INKWELL_FIELD_U8:
        member[0] = (uint8_t)value;
        return;
    case INKWELL_FIELD_U16: {
        const uint16_t v = (uint16_t)value;
        memcpy(member, &v, sizeof v);
        return;
    }
    case INKWELL_FIELD_U32:
    case INKWELL_FIELD_I32: {
        const uint32_t v = (uint32_t)value;
        memcpy(member, &v, sizeof v);
        return;
    }
    case INKWELL_FIELD_U64:
    case INKWELL_FIELD_I64:
        memcpy(member, &value, sizeof value);
        return;
    case INKWELL_FIELD_STR:
        return;
    }
}

size_t inkwell_fields_encoded_max(const struct inkwell_field *fields, size_t count) {
    if (fields == NULL && count > 0U) {
        return 0U;
    }
    size_t total = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (!field_valid(&fields[i])) {
            return 0U;
        }
        const size_t width = field_width(fields[i].type);
        total += width != 0U ? width : 2U + (size_t)fields[i].size - 1U;
    }
    return total;
}

int inkwell_fields_encode(const struct inkwell_field *fields, size_t count, const void *record,
                          uint8_t *out, size_t out_len) {
    if ((fields == NULL && count > 0U) || (record == NULL && count > 0U) ||
        (out == NULL && out_len > 0U)) {
        return -EINVAL;
    }
    const uint8_t *base = record;
    size_t at = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const struct inkwell_field *field = &fields[i];
        if (!field_valid(field)) {
            return -EINVAL;
        }
        const uint8_t *member = base + field->offset;
        const size_t width = field_width(field->type);
        if (width != 0U) {
            if (out_len - at < width) {
                return -EMSGSIZE;
            }
            put_le(out + at, load(member, field->type), width);
            at += width;
            continue;
        }
        const void *nul = memchr(member, '\0', field->size);
        if (nul == NULL) {
            return -EINVAL; /* a member with no end is not a string */
        }
        const size_t text = (size_t)((const uint8_t *)nul - member);
        if (out_len - at < 2U + text) {
            return -EMSGSIZE;
        }
        put_le(out + at, text, 2U);
        memcpy(out + at + 2U, member, text);
        at += 2U + text;
    }
    return at <= (size_t)INT_MAX ? (int)at : -EMSGSIZE;
}

/* Text that is UTF-8 all the way through and has no NUL inside it. */
static bool text_valid(const uint8_t *text, size_t len) {
    size_t at = 0U;
    while (at < len) {
        if (text[at] == 0U) {
            return false;
        }
        const size_t step = inkwell_text_utf8_sequence_len(text + at, len - at);
        if (step == 0U) {
            return false;
        }
        at += step;
    }
    return true;
}

static int decode_fields(const struct inkwell_field *fields, size_t count, const uint8_t *in,
                         size_t len, uint8_t *base) {
    size_t at = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const struct inkwell_field *field = &fields[i];
        if (at == len) {
            return (int)i; /* an older peer: the fields it did not know stay zero */
        }
        uint8_t *member = base + field->offset;
        const size_t width = field_width(field->type);
        if (width != 0U) {
            if (len - at < width) {
                return -EBADMSG;
            }
            const uint64_t value = get_le(in + at, width);
            if (field->type == INKWELL_FIELD_BOOL && value > 1U) {
                return -EBADMSG;
            }
            store(member, field->type, value);
            at += width;
            continue;
        }
        if (len - at < 2U) {
            return -EBADMSG;
        }
        const size_t text = (size_t)get_le(in + at, 2U);
        if (len - at - 2U < text) {
            return -EBADMSG;
        }
        if (text >= field->size) {
            return -EMSGSIZE;
        }
        if (!text_valid(in + at + 2U, text)) {
            return -EBADMSG;
        }
        memcpy(member, in + at + 2U, text);
        member[text] = '\0';
        at += 2U + text;
    }
    return (int)count; /* anything after the last field is a newer peer's, and not ours */
}

/* Every member the table names, zeroed; what lies between and beyond them is the caller's. */
static void zero_members(const struct inkwell_field *fields, size_t count, uint8_t *base) {
    for (size_t i = 0U; i < count; ++i) {
        memset(base + fields[i].offset, 0, fields[i].size);
    }
}

int inkwell_fields_decode(const struct inkwell_field *fields, size_t count, const uint8_t *in,
                          size_t len, void *record) {
    if ((fields == NULL && count > 0U) || record == NULL || (in == NULL && len > 0U)) {
        return -EINVAL;
    }
    /* Every row is checked before any byte of the record is written: a bad row is a bad table
       whatever the message, not only when a message is long enough to reach it. */
    for (size_t i = 0U; i < count; ++i) {
        if (!field_valid(&fields[i])) {
            return -EINVAL;
        }
    }
    /* Only the members the table names are zeroed: the caller's struct may be larger (a newer
       layout read through an older table) or hold members that never go on the wire, and what
       this table does not name is not ours to touch. */
    zero_members(fields, count, record);
    const int result = decode_fields(fields, count, in, len, record);
    if (result < 0) {
        zero_members(fields, count, record);
    }
    return result;
}
