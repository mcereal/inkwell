/*
 * Internal seam between report layout and platform backends. The backend supplies the fault,
 * image identity, and a frame writer; crash_report.c owns all text and section ordering.
 */
#pragma once

#include "inkwell/runtime/crash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CrashFault {
    int signal_number;
    const char *signal_name;
    bool has_info;
    int64_t code;
    uint64_t fault_address;
    bool have_registers;
    uint64_t pc;
    uint64_t fp;
    const char *load_base;
    const char *build_id;
    const char *image_size;
} CrashFault;

typedef void (*CrashWriteFrames)(int fd, uint64_t fp);

/* Minimal file operations keep the writer and report state independent of the host CRT. */
ptrdiff_t inkwell_crash_internal_backend_write(int fd, const char *data, size_t len);
bool inkwell_crash_internal_path_exists(const char *path);
int inkwell_crash_internal_unlink(const char *path);

int inkwell_crash_internal_prepare(const struct inkwell_crash_config *config);
const char *inkwell_crash_internal_path(void);
void inkwell_crash_internal_write_report(int fd, const CrashFault *fault,
                                         CrashWriteFrames write_frames);

void inkwell_crash_internal_write(int fd, const char *data, size_t len);
void inkwell_crash_internal_puts(int fd, const char *text);
size_t inkwell_crash_internal_format_unsigned(char *out, size_t out_len, uint64_t value,
                                              unsigned base, size_t pad);
void inkwell_crash_internal_write_address(int fd, uint64_t value);
