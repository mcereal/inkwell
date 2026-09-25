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

/*
 * Makes one directory, and only one: the parent has to exist already. 0 when it was made,
 * -EEXIST when something is already there, or another negative errno.
 *
 * -EEXIST is reported rather than folded into success because "something is there" is not "a
 * directory is there"; a caller that only wants somewhere to write treats it as success and
 * finds out on the first open, which is where a file in the way would have surfaced anyway.
 *
 * Private to its owner on POSIX (0700): what a program keeps in a directory of its own - a
 * journal, a cache, a list of peers it has paired with - is nobody else's to read. Windows has
 * no mode to give, so the directory inherits its parent's ACL.
 */
int inkwell_file_mkdir(const char *path);

#ifdef __cplusplus
}
#endif
