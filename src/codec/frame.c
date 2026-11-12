#include "inkwell/codec/frame.h"

#include <errno.h>
#include <string.h>

int inkwell_frame_header(uint8_t out[INKWELL_FRAME_HEADER], size_t len) {
    if (out == NULL) {
        return -EINVAL;
    }
    if ((uint64_t)len > UINT32_MAX) {
        return -EMSGSIZE;
    }
    for (size_t i = 0U; i < INKWELL_FRAME_HEADER; ++i) {
        out[i] = (uint8_t)((uint64_t)len >> (8U * i));
    }
    return 0;
}

void inkwell_frame_reader_init(struct inkwell_frame_reader *reader, uint8_t *buffer,
                               size_t capacity) {
    if (reader == NULL) {
        return;
    }
    memset(reader, 0, sizeof *reader);
    reader->buffer = buffer;
    reader->capacity = buffer != NULL ? capacity : 0U;
}

int inkwell_frame_reader_feed(struct inkwell_frame_reader *reader, const uint8_t *bytes, size_t len,
                              inkwell_frame_fn on_frame, void *userdata) {
    if (reader == NULL || (bytes == NULL && len > 0U)) {
        return -EINVAL;
    }
    size_t at = 0U;
    for (;;) {
        if (reader->broken) {
            return -EMSGSIZE;
        }
        if (reader->header_have < INKWELL_FRAME_HEADER) {
            const size_t need = INKWELL_FRAME_HEADER - reader->header_have;
            const size_t take = len - at < need ? len - at : need;
            if (take > 0U) {
                memcpy(reader->header + reader->header_have, bytes + at, take);
            }
            reader->header_have += take;
            at += take;
            if (reader->header_have < INKWELL_FRAME_HEADER) {
                return 0;
            }
            uint64_t want = 0U;
            for (size_t i = 0U; i < INKWELL_FRAME_HEADER; ++i) {
                want |= (uint64_t)reader->header[i] << (8U * i);
            }
            if (want > reader->capacity) {
                reader->broken = true;
                return -EMSGSIZE;
            }
            reader->want = (size_t)want;
            reader->have = 0U;
        }
        const size_t need = reader->want - reader->have;
        const size_t take = len - at < need ? len - at : need;
        if (take > 0U) {
            memcpy(reader->buffer + reader->have, bytes + at, take);
            reader->have += take;
            at += take;
        }
        if (reader->have < reader->want) {
            return 0;
        }
        /* Whole. Ready for the next header before the callback runs, so a callback that feeds
           nothing and returns leaves the reader in a clean state. */
        const size_t frame_len = reader->want;
        reader->header_have = 0U;
        reader->want = 0U;
        reader->have = 0U;
        if (on_frame != NULL) {
            const int stop = on_frame(userdata, reader->buffer, frame_len);
            if (stop != 0) {
                return stop;
            }
        }
        if (at == len) {
            return 0;
        }
    }
}
