#ifndef INKWELL_BASE_RECORD_FILE_H
#define INKWELL_BASE_RECORD_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small line-record files are useful when the application owns the schema but needs a durable
 * file on removable storage. The caller owns key names, field encoding and record boundaries;
 * this layer only handles escaped values and file replacement. A callback sees one decoded
 * key/value pair at a time. Unknown keys are therefore the caller's decision.
 */
typedef void (*inkwell_record_visit_fn)(void *context, const char *key, char *value);
typedef void (*inkwell_record_write_fn)(FILE *file, void *context);

/* Read newline-delimited key=value lines. The caller supplies the fixed line buffer. Blank
 * lines, comments and lines without '=' are ignored. An overlong line is discarded whole, so
 * its tail cannot become a forged record. Returns zero or a negative errno. */
int inkwell_record_read(FILE *file, char *line, size_t capacity, inkwell_record_visit_fn visit,
                        void *context);

/* Escape control bytes, backslash and '=' as \xNN; decode them in place on read. */
void inkwell_record_write_escaped(FILE *file, const char *value);
void inkwell_record_unescape(char *value);

/* Write beside path and rename only after a successful close. `sync_data` flushes the file to
 * storage before publishing it, for snapshots that must survive sudden power loss. A writer
 * reports failures through ferror(file). The temporary path is caller-owned scratch space. */
int inkwell_record_replace(const char *path, char *temp, size_t temp_capacity,
                           inkwell_record_write_fn write_records, void *context, bool sync_data);

/* Append and close on each call; a reader can ignore an incomplete final record. */
int inkwell_record_append(const char *path, inkwell_record_write_fn write_records, void *context);

#ifdef __cplusplus
}
#endif

#endif
