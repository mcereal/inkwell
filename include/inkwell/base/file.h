#pragma once

#include <stdbool.h>
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

/*
 * Moves `from` over `to`, replacing whatever `to` was: 0 or a negative errno.
 *
 * rename() does this on POSIX and refuses on Windows when `to` exists, which is exactly the case
 * a writer through a temporary is in - so a program that renamed directly worked everywhere it
 * was tested and nowhere a user on Windows ran it twice. Same directory, same filesystem: a
 * reader opening `to` sees the old file or the new one. inkwell_record_replace() is this plus
 * the write; this is for a writer that decides only after writing whether to publish at all.
 */
int inkwell_file_replace(const char *from, const char *to);

/*
 * Whether `path` names a directory - not merely something. inkwell_file_mkdir()'s -EEXIST is the
 * case this is for: a file sitting where a program meant to keep a directory would otherwise be
 * taken for one, and every open inside it would fail later, far from the reason.
 */
bool inkwell_file_is_dir(const char *path);

/*
 * Calls `visit` once for every entry in `dir` by name, "." and ".." aside, in whatever order the
 * system keeps them. 0, or a negative errno when the directory cannot be listed (-ENOTDIR for a
 * file). The name is the entry's alone; the caller joins it to `dir`.
 *
 * A visitor may remove the entry it was handed - a wipe of a program's own files is the reason
 * this exists - and the walk carries on; it may not rely on seeing an entry created meanwhile.
 */
typedef void (*inkwell_file_entry_fn)(void *context, const char *name);

int inkwell_file_list(const char *dir, inkwell_file_entry_fn visit, void *context);

#ifdef __cplusplus
}
#endif
