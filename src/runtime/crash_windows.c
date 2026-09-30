#include "crash_internal.h"

#include <errno.h>
#include <io.h>
#include <string.h>

ptrdiff_t crash_backend_write(int fd, const char *data, size_t len) {
    return (ptrdiff_t)_write(fd, data, (unsigned)len);
}

bool crash_backend_path_exists(const char *path) {
    return _access(path, 0) == 0;
}

int crash_backend_unlink(const char *path) {
    return _unlink(path);
}

int inkwell_crash_install(const struct inkwell_crash_config *config) {
    if (config == NULL || config->dir == NULL || config->product == NULL ||
        config->log_warning == NULL) {
        return -EINVAL;
    }
    return -ENOTSUP;
}

void inkwell_crash_write_report(int fd, int signal_number) {
    (void)signal_number;
    static const char message[] = "Crash reporting is not available on Windows yet.\n";
    if (fd >= 0) {
        (void)_write(fd, message, (unsigned)(sizeof message - 1U));
    }
}
