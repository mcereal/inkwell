#include "inkwell/io/serial.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct inkwell_serial_unavailable_mock {
    bool enabled;
    struct inkwell_serial_mock_config config;
    size_t bind_calls;
    size_t line_state_calls;
    unsigned bind_pending_left;
};

static struct inkwell_serial_unavailable_mock g_mock;

/* This host does not yet have a device-discovery or serial-port backend. Do not report a port
 * that cannot be opened, and keep the refusal distinct from a missing or unplugged device. */
size_t inkwell_serial_scan(struct inkwell_serial_port_info *out, size_t capacity) {
    if (out == NULL || capacity == 0U || !g_mock.enabled || g_mock.config.scan_result < 0 ||
        g_mock.config.ports == NULL) {
        return 0U;
    }
    const size_t count = g_mock.config.port_count < capacity ? g_mock.config.port_count : capacity;
    memcpy(out, g_mock.config.ports, count * sizeof *out);
    return count;
}

int inkwell_serial_bind(struct inkwell_serial_port_info *port) {
    if (port == NULL) {
        return -EINVAL;
    }
    if (g_mock.enabled) {
        g_mock.bind_calls += 1U;
        if (g_mock.config.bind_result < 0) {
            return g_mock.config.bind_result;
        }
        if (!port->bound && g_mock.bind_pending_left > 0U) {
            g_mock.bind_pending_left -= 1U;
            port->bind_requested = true;
            return -EAGAIN;
        }
        if (!port->bound) {
            const char *path = g_mock.config.bound_path != NULL ? g_mock.config.bound_path : "COM1";
            (void)snprintf(port->path, sizeof port->path, "%s", path);
            port->bound = true;
        }
        return 0;
    }
    return -ENOTSUP;
}

int inkwell_serial_set_line_state(const struct inkwell_serial_port_info *port, bool dtr, bool rts) {
    (void)dtr;
    (void)rts;
    if (port == NULL) {
        return -EINVAL;
    }
    if (g_mock.enabled) {
        g_mock.line_state_calls += 1U;
        return g_mock.config.line_state_result;
    }
    return -ENOTSUP;
}

int inkwell_serial_open(const char *path, unsigned baud) {
    (void)baud;
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    if (g_mock.enabled) {
        if (g_mock.config.open_result < 0) {
            return g_mock.config.open_result;
        }
        return g_mock.config.open_fd >= 0 ? inkwell_fd_dup(g_mock.config.open_fd) : -ENOENT;
    }
    return -ENOTSUP;
}

void inkwell_serial_close(int fd) {
    if (fd >= 0) {
        (void)inkwell_fd_close(fd);
    }
}

int inkwell_serial_set_dtr(int fd, bool on) {
    (void)on;
    return fd < 0 ? -EINVAL : -ENOTSUP;
}

void inkwell_serial_set_sysfs_root(const char *root) {
    (void)root;
}

void inkwell_serial_mock_enable(const struct inkwell_serial_mock_config *config) {
    memset(&g_mock, 0, sizeof g_mock);
    g_mock.enabled = true;
    if (config != NULL) {
        g_mock.config = *config;
        g_mock.bind_pending_left = config->bind_pending_polls;
    } else {
        g_mock.config.open_fd = -1;
    }
}

void inkwell_serial_mock_disable(void) {
    memset(&g_mock, 0, sizeof g_mock);
}

size_t inkwell_serial_mock_bind_calls(void) {
    return g_mock.bind_calls;
}

size_t inkwell_serial_mock_line_state_calls(void) {
    return g_mock.line_state_calls;
}
