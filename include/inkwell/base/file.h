#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads a whole file into a fresh buffer the caller frees. NULL when the file is missing,
 * unreadable, empty, or longer than `max_len` - which is a refusal rather than a truncation,
 * because a caller hands the bytes on whole - to a decoder, a peer, a flasher - and half of one
 * is the outcome worth a check to avoid.
 */
uint8_t *inkwell_file_read(const char *path, size_t max_len, size_t *out_len);

#ifdef __cplusplus
}
#endif
