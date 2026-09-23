#include "inkwell/runtime/crash.h"

#include <errno.h>
#include <io.h>
#include <string.h>

int inkwell_crash_install(const struct inkwell_crash_config *config) {
    if (config == NULL || config->dir == NULL || config->product == NULL ||
        config->log_warning == NULL) {
        return -EINVAL;
    }
    return -ENOTSUP;
}

bool inkwell_crash_report_path(char *out, size_t out_len) {
    if (out != NULL && out_len > 0U) {
        out[0] = '\0';
    }
    return false;
}

bool inkwell_crash_report_waiting(void) {
    return false;
}

int inkwell_crash_discard(void) {
    return 0;
}

void inkwell_crash_note(unsigned slot, const char *value) {
    (void)slot;
    (void)value;
}

void inkwell_crash_log_line(const char *line) {
    (void)line;
}

void inkwell_crash_write_report(int fd, int signal_number) {
    (void)signal_number;
    static const char message[] = "Crash reporting is not available on Windows yet.\n";
    if (fd >= 0) {
        (void)_write(fd, message, (unsigned)(sizeof message - 1U));
    }
}
