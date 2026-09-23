#include "inkwell/ble/central.h"

#include "central_internal.h"
#include "hci.h"

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* A read that has had no answer in this long is abandoned. */
#define INKWELL_BLE_READ_TIMEOUT_MS 3000U
/*
 * Generous on purpose, and deliberately unlike the read's.
 *
 * This bounds an *asynchronous* property query. Nothing waits on the answer, so the deadline
 * does not decide how responsive anything is - it only decides when a request whose reply may
 * never come is stale enough to stop tracking. Whether a link is hopeless is the caller's
 * question, on a clock of its own, and a query that expires here can simply be asked again.
 *
 * A second was too tight to be that: bluetoothd was measured on a small ARM handheld answering
 * one of these 1036 ms after it was issued, mid-connect, and every poll of a connect expiring
 * is how a link BlueZ had resolved fine never came up at all.
 */
#define INKWELL_BLE_PROPERTY_TIMEOUT_MS 5000U

#define INKWELL_BLE_MOCK_PAIRED_MAX 4U

/* ---- the mock ------------------------------------------------------------------------------ */

struct inkwell_ble_mock_state {
    bool enabled;
    struct inkwell_ble_mock_config config;
    struct inkwell_ble_central *central;
    size_t read_cursor;
    unsigned services_resolved_polls;
    unsigned services_resolved_timeouts;
    unsigned connect_polls;
    unsigned connected_polls;
    /* Whether a scan is up, for config.connect_needs_the_scan. Starts false: a mock that has
       never been told to scan has nothing that could have been dropped by stopping one. */
    bool scanning;
    unsigned write_calls;
    unsigned pair_polls;
    /* A subscribe in flight, and the calls it has answered -EAGAIN to. */
    bool subscribing;
    unsigned subscribe_polls;
    /* Addresses the mock has bonded, so a device lists as paired the way it would on the
       stack once the pairing completed. */
    char paired[INKWELL_BLE_MOCK_PAIRED_MAX][INKWELL_BLE_ADDRESS_MAX];
    size_t paired_count;
};

static struct inkwell_ble_mock_state g_mock;

/* Everything the mock answers, including the calls the bus seam leaves to it. */
static bool mocked(void) {
    return g_mock.enabled;
}

/* Everything the mock answers when there is no bus behind it: reads, writes, property queries,
   the lookups, discovery, disconnect, trust and the loop. With a bus address those go to the
   real backend. */
static bool scripted(void) {
    return g_mock.enabled && g_mock.config.bus_address == NULL;
}

static void mock_note_paired(const char *address) {
    if (address == NULL || address[0] == '\0' ||
        g_mock.paired_count >= INKWELL_BLE_MOCK_PAIRED_MAX) {
        return;
    }
    inkwell_str_copy(g_mock.paired[g_mock.paired_count], sizeof g_mock.paired[0], address);
    g_mock.paired_count++;
}

static bool mock_is_paired(const char *address) {
    for (size_t i = 0; i < g_mock.paired_count; ++i) {
        if (strcmp(g_mock.paired[i], address) == 0) {
            return true;
        }
    }
    return false;
}

static void mock_list(const char *service_uuid, struct inkwell_ble_device *devices, size_t capacity,
                      size_t *count) {
    *count = 0U;
    if (g_mock.config.devices == NULL) {
        return;
    }
    const char *const *services = g_mock.config.device_service_uuids;
    size_t copied = 0U;
    for (size_t i = 0; i < g_mock.config.device_count && copied < capacity; ++i) {
        if (services != NULL && service_uuid != NULL &&
            (services[i] == NULL || strcasecmp(services[i], service_uuid) != 0)) {
            continue;
        }
        struct inkwell_ble_device *const device = &devices[copied++];
        *device = g_mock.config.devices[i];
        if (!device->paired && mock_is_paired(device->address)) {
            device->paired = true;
        }
        /* A mock device that names a signal strength is saying it was heard, exactly as the
           RSSI property does on BlueZ. A test that wants a bond with nothing behind it - the
           peripheral left at home - writes the 0 the stack leaves behind. */
        if (!device->in_range && device->rssi != 0) {
            device->in_range = true;
        }
    }
    *count = copied;
}

/* Everything but `enabled` and `config`: the per-run counters a test would otherwise see
   carried over from the test before it. */
static void mock_reset_counters(void) {
    g_mock.central = NULL;
    g_mock.read_cursor = 0U;
    g_mock.services_resolved_polls = 0U;
    g_mock.services_resolved_timeouts = 0U;
    g_mock.connect_polls = 0U;
    g_mock.connected_polls = 0U;
    g_mock.scanning = false;
    g_mock.write_calls = 0U;
    g_mock.subscribing = false;
    g_mock.subscribe_polls = 0U;
    g_mock.pair_polls = 0U;
    memset(g_mock.paired, 0, sizeof g_mock.paired);
    g_mock.paired_count = 0U;
}

void inkwell_ble_mock_enable(const struct inkwell_ble_mock_config *config) {
    g_mock.enabled = true;
    if (config != NULL) {
        g_mock.config = *config;
    } else {
        memset(&g_mock.config, 0, sizeof g_mock.config);
    }
    mock_reset_counters();
}

void inkwell_ble_mock_disable(void) {
    g_mock.enabled = false;
    memset(&g_mock.config, 0, sizeof g_mock.config);
    mock_reset_counters();
}

void inkwell_ble_mock_emit_notification(const char *handle, const uint8_t *data, size_t len) {
    if (!g_mock.enabled) {
        return;
    }
    struct inkwell_ble_central *central = g_mock.central;
    if (central == NULL || data == NULL || len == 0U) {
        return;
    }
    if (handle != NULL && central->notify_handle[0] != '\0' &&
        strcmp(handle, central->notify_handle) != 0) {
        return;
    }
    inkwell_ble_notify(central, data, len);
}

static int mock_discovery(bool on) {
    if (on) {
        if (g_mock.config.start_discovery_calls != NULL) {
            ++*g_mock.config.start_discovery_calls;
        }
        if (g_mock.config.start_discovery_result == 0) {
            g_mock.scanning = true;
        }
        return g_mock.config.start_discovery_result;
    }
    if (g_mock.config.stop_discovery_calls != NULL) {
        ++*g_mock.config.stop_discovery_calls;
    }
    if (g_mock.config.stop_discovery_result == 0) {
        g_mock.scanning = false;
    }
    return g_mock.config.stop_discovery_result;
}

static int mock_write(const char *handle, const uint8_t *data, size_t len) {
    const struct inkwell_ble_mock_config *cfg = &g_mock.config;
    size_t call_index = 0U;
    if (cfg->write_call_count != NULL) {
        (*cfg->write_call_count)++;
        call_index = *cfg->write_call_count;
    }
    if (cfg->write_lengths != NULL && call_index > 0U &&
        (call_index - 1U) < cfg->write_lengths_capacity) {
        cfg->write_lengths[call_index - 1U] = len;
    }
    if (cfg->write_capture_buffer != NULL && cfg->write_capture_length != NULL) {
        const size_t to_copy =
            len > cfg->write_capture_capacity ? cfg->write_capture_capacity : len;
        if (to_copy > 0U) {
            memcpy(cfg->write_capture_buffer, data, to_copy);
        }
        *cfg->write_capture_length = to_copy;
    }
    if (cfg->write_capture_handle != NULL && cfg->write_capture_handle_capacity > 0U) {
        inkwell_str_copy(cfg->write_capture_handle, cfg->write_capture_handle_capacity, handle);
    }

    g_mock.write_calls++;
    int result = cfg->write_result;
    if (cfg->write_fail_after_calls != 0U && g_mock.write_calls > cfg->write_fail_after_calls) {
        result = cfg->write_result_late;
    }
    if (result == 0 && cfg->write_hook != NULL) {
        cfg->write_hook(cfg->write_hook_userdata, handle, data, len);
    }
    return result;
}

/* ---- request bookkeeping ------------------------------------------------------------------- */

void inkwell_ble_read_cancel(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return;
    }
    if (central->read_state != 0 && central->read_timer_fd >= 0) {
        if (central->loop != NULL) {
            inkwell_loop_remove_fd(central->loop, central->read_timer_fd);
        }
        close(central->read_timer_fd);
    }
    central->read_timer_fd = -1;
    central->read_state = 0;
    central->read_token = 0U;
    central->read_length = 0U;
    central->read_deadline_ms = 0U;
    central->read_mock_polls = 0U;
}

void inkwell_ble_read_finish(struct inkwell_ble_central *central, int result) {
    if (central->read_state != 1) {
        return;
    }
    central->read_state = 2;
    central->read_result = result;
    central->read_token = 0U; /* a late reply after a timeout is ignored */
    if (central->read_ready != NULL) {
        central->read_ready(central->userdata);
    }
}

static int read_timeout(int fd, uint32_t events, void *userdata) {
    (void)events;
    if (inkwell_timer_read(fd) <= 0) {
        return 0;
    }
    inkwell_ble_read_finish(userdata, -ETIMEDOUT);
    return 0;
}

static int read_start(struct inkwell_ble_central *central) {
    central->read_state = 1;
    central->read_deadline_ms = inkwell_time_monotonic_ms() + INKWELL_BLE_READ_TIMEOUT_MS;
    if (central->loop == NULL) {
        return 0;
    }
    const int fd = inkwell_timer_open();
    if (fd < 0) {
        inkwell_ble_read_cancel(central);
        return fd;
    }
    central->read_timer_fd = fd;
    int result = inkwell_loop_add_fd(central->loop, fd, INKWELL_LOOP_IN, read_timeout, central);
    if (result == 0) {
        result = inkwell_timer_arm_once(fd, INKWELL_BLE_READ_TIMEOUT_MS);
    }
    if (result < 0) {
        inkwell_ble_read_cancel(central);
    }
    return result;
}

static void pending_cancel(struct inkwell_ble_pending *request) {
    if (request->state != 0 && request->timer_fd >= 0) {
        if (request->central != NULL && request->central->loop != NULL) {
            inkwell_loop_remove_fd(request->central->loop, request->timer_fd);
        }
        close(request->timer_fd);
    }
    request->state = 0;
    request->token = 0U;
    request->timer_fd = -1;
}

void inkwell_ble_requests_cancel(struct inkwell_ble_central *central) {
    if (scripted()) {
        g_mock.subscribing = false;
    }
    if (central != NULL) {
        for (size_t i = 0; i < INKWELL_ARRAY_LEN(central->requests); ++i) {
            pending_cancel(&central->requests[i]);
        }
    }
}

void inkwell_ble_pending_finish(struct inkwell_ble_pending *request, int result) {
    if (request->state != 1) {
        return;
    }
    request->result = result;
    request->state = 2;
    request->token = 0U;
    struct inkwell_ble_central *central = request->central;
    if (central != NULL && central->requests_ready != NULL) {
        central->requests_ready(central->userdata);
    }
}

static int pending_timeout(int fd, uint32_t events, void *userdata) {
    (void)events;
    if (inkwell_timer_read(fd) > 0) {
        inkwell_ble_pending_finish(userdata, -ETIMEDOUT);
    }
    return 0;
}

static int pending_start(struct inkwell_ble_central *central, struct inkwell_ble_pending *request,
                         unsigned timeout_ms) {
    request->central = central;
    request->timer_fd = -1;
    request->state = 1;
    request->token = 0U;
    request->deadline_ms = inkwell_time_monotonic_ms() + timeout_ms;
    if (central->loop == NULL) {
        return 0;
    }
    int result = inkwell_timer_open();
    if (result >= 0) {
        request->timer_fd = result;
        result = inkwell_loop_add_fd(central->loop, request->timer_fd, INKWELL_LOOP_IN,
                                     pending_timeout, request);
        if (result == 0) {
            result = inkwell_timer_arm_once(request->timer_fd, timeout_ms);
        }
    }
    if (result < 0) {
        pending_cancel(request);
    }
    return result;
}

/* The answer to a request that is done, which also retires it; -EAGAIN while it is not. */
static int pending_take(struct inkwell_ble_pending *request) {
    if (request->state == 1) {
        return -EAGAIN;
    }
    const int result = request->result;
    pending_cancel(request);
    return result;
}

void inkwell_ble_connect_finish(struct inkwell_ble_central *central, int result) {
    if (central->connect_state != 1) {
        return;
    }
    central->connect_result = result;
    central->connect_state = 2;
}

void inkwell_ble_pair_finish(struct inkwell_ble_central *central, int result) {
    if (central->pair_state != 1) {
        return;
    }
    central->pair_result = result;
    central->pair_state = 2;
}

void inkwell_ble_notify(struct inkwell_ble_central *central, const uint8_t *data, size_t len) {
    if (central->notification_callback != NULL && data != NULL && len > 0U) {
        central->notification_callback(data, len, central->notification_userdata);
    }
}

/* ---- lifecycle ----------------------------------------------------------------------------- */

static int central_open(struct inkwell_ble_central *central, bool private_connection) {
    if (central == NULL) {
        return -EINVAL;
    }
    memset(central, 0, sizeof *central);
    central->read_timer_fd = -1;
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(central->requests); ++i) {
        central->requests[i].central = central;
        central->requests[i].timer_fd = -1;
    }

    if (scripted()) {
        if (g_mock.config.open_result < 0) {
            return g_mock.config.open_result;
        }
        central->open = true;
        g_mock.central = central;
        return 0;
    }

    const int result = inkwell_ble_backend_open(central, private_connection,
                                                mocked() ? g_mock.config.bus_address : NULL);
    if (result < 0) {
        return result;
    }
    central->open = true;
    return 0;
}

int inkwell_ble_open(struct inkwell_ble_central *central) {
    return central_open(central, false);
}

int inkwell_ble_open_private(struct inkwell_ble_central *central) {
    return central_open(central, true);
}

void inkwell_ble_close(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return;
    }
    inkwell_ble_read_cancel(central);
    inkwell_ble_requests_cancel(central);

    /* Give the stack its agent back before the connection goes: a registration left behind on
       a name that has vanished blocks the next one. */
    inkwell_ble_agent_unregister(central);
    central->pair_state = 0;
    central->pair_token = 0U;
    central->pair_address[0] = '\0';

    if (central->loop != NULL) {
        inkwell_ble_detach_loop(central);
    }
    if (central->backend != NULL) {
        inkwell_ble_backend_close(central);
    }
    central->backend = NULL;
    central->open = false;
    central->loop = NULL;
    central->notification_callback = NULL;
    central->notification_userdata = NULL;
    central->notify_handle[0] = '\0';
    if (g_mock.central == central) {
        g_mock.central = NULL;
    }
}

int inkwell_ble_check_ready(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (mocked()) {
        return g_mock.config.check_ready_result;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_check_ready(central);
}

int inkwell_ble_find_adapter(struct inkwell_ble_central *central, char *name, size_t name_len) {
    if (central == NULL || name == NULL || name_len == 0U) {
        return -EINVAL;
    }
    if (scripted()) {
        if (g_mock.config.find_adapter_result < 0) {
            return g_mock.config.find_adapter_result;
        }
        const char *adapter =
            g_mock.config.adapter_name != NULL ? g_mock.config.adapter_name : "/org/bluez/hci0";
        inkwell_str_copy(central->adapter, sizeof central->adapter, adapter);
        inkwell_str_copy(name, name_len, adapter);
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_find_adapter(central, name, name_len);
}

int inkwell_ble_request_connection_interval(
    struct inkwell_ble_central *central, const char *address,
    const struct inkwell_ble_connection_parameters *parameters) {
    if (central == NULL || address == NULL || address[0] == '\0' ||
        !inkwell_ble_hci_parameters_valid(parameters)) {
        return -EINVAL;
    }
    if (scripted()) {
        if (g_mock.config.request_connection_interval_calls != NULL) {
            ++*g_mock.config.request_connection_interval_calls;
        }
        return g_mock.config.request_connection_interval_result;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_request_connection_interval(central, address, parameters);
}

int inkwell_ble_start_discovery(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        return mock_discovery(true);
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_discovery(central, true);
}

int inkwell_ble_stop_discovery(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        return mock_discovery(false);
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_discovery(central, false);
}

int inkwell_ble_list_by_service(struct inkwell_ble_central *central, const char *service_uuid,
                                struct inkwell_ble_device *devices, size_t capacity,
                                size_t *count) {
    if (central == NULL || service_uuid == NULL || devices == NULL || count == NULL) {
        return -EINVAL;
    }
    *count = 0U;
    if (capacity == 0U) {
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    if (scripted()) {
        if (g_mock.config.list_calls != NULL) {
            ++*g_mock.config.list_calls;
        }
        mock_list(service_uuid, devices, capacity, count);
        return g_mock.config.list_result;
    }
    return inkwell_ble_backend_list_by_service(central, service_uuid, devices, capacity, count);
}

/* ---- connect ------------------------------------------------------------------------------- */

int inkwell_ble_connect_begin(struct inkwell_ble_central *central, const char *address) {
    if (central == NULL || address == NULL) {
        return -EINVAL;
    }
    if (central->connect_state == 1) {
        return -EBUSY;
    }
    if (mocked()) {
        g_mock.connect_polls = 0U;
        central->connect_state = 1;
        central->connect_token = 1U;
        /* The device went with the scan: BlueZ answers UnknownObject, which maps to -ENOENT,
           and it arrives on the poll like any other reply. */
        central->connect_result = g_mock.config.connect_needs_the_scan && !g_mock.scanning
                                      ? -ENOENT
                                      : g_mock.config.connect_result;
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    uint32_t token = 0U;
    const int result = inkwell_ble_backend_connect(central, address, &token);
    if (result < 0) {
        return result;
    }
    central->connect_token = token;
    central->connect_state = 1;
    central->connect_result = 0;
    return 0;
}

int inkwell_ble_connect_poll(struct inkwell_ble_central *central, int *out_result) {
    if (central == NULL || out_result == NULL) {
        return -EINVAL;
    }
    if (central->connect_state == 0) {
        return -EINVAL;
    }
    if (mocked() && central->connect_state == 1) {
        g_mock.connect_polls++;
        if (g_mock.connect_polls > g_mock.config.connect_pending_polls) {
            central->connect_state = 2;
            if (central->connect_result == 0) {
                g_mock.central = central;
            }
        }
    }
    if (central->connect_state != 2) {
        return 0;
    }
    *out_result = central->connect_result;
    central->connect_state = 0;
    central->connect_token = 0U;
    return 1;
}

void inkwell_ble_connect_cancel(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return;
    }
    if (!mocked() && central->connect_state == 1 && central->backend != NULL) {
        inkwell_ble_backend_connect_cancel(central);
    }
    central->connect_state = 0;
    central->connect_token = 0U;
    central->connect_result = 0;
}

int inkwell_ble_disconnect(struct inkwell_ble_central *central, const char *address) {
    if (central == NULL || address == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        const int result = g_mock.config.disconnect_result;
        if (result == 0 && g_mock.central == central) {
            g_mock.central = NULL;
            central->notify_handle[0] = '\0';
        }
        return result;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    const int result = inkwell_ble_backend_disconnect(central, address);
    if (result == 0) {
        central->notify_handle[0] = '\0';
    }
    return result;
}

/* ---- pairing ------------------------------------------------------------------------------- */

int inkwell_ble_pair_begin(struct inkwell_ble_central *central, const char *address) {
    if (central == NULL || address == NULL) {
        return -EINVAL;
    }
    if (central->pair_state == 1) {
        return -EBUSY;
    }
    inkwell_str_copy(central->pair_address, sizeof central->pair_address, address);

    if (mocked()) {
        g_mock.pair_polls = 0U;
        central->pair_state = 1;
        central->pair_token = 1U;
        central->pair_result = g_mock.config.pair_result;
        if (g_mock.config.pair_requests_passkey) {
            central->agent_request.kind = INKWELL_BLE_AGENT_REQUEST_PASSKEY;
            inkwell_str_copy(central->agent_request.address, sizeof central->agent_request.address,
                             address);
            central->agent_request.passkey = 0U;
        }
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    uint32_t token = 0U;
    const int result = inkwell_ble_backend_pair(central, address, &token);
    if (result < 0) {
        return result;
    }
    central->pair_token = token;
    central->pair_state = 1;
    central->pair_result = 0;
    inkwell_log_info("ble", "Pairing with %s", address);
    return 0;
}

int inkwell_ble_pair_poll(struct inkwell_ble_central *central, int *out_result) {
    if (central == NULL || out_result == NULL) {
        return -EINVAL;
    }
    if (central->pair_state == 0) {
        return -EINVAL;
    }
    if (mocked() && central->pair_state == 1) {
        /* A stack does not finish a pair while its agent is waiting on the user, and neither
           does the mock: a test that never submits a passkey sees the pair stay pending. */
        if (central->agent_request.kind == INKWELL_BLE_AGENT_REQUEST_NONE) {
            g_mock.pair_polls++;
            if (g_mock.pair_polls > g_mock.config.pair_pending_polls) {
                central->pair_state = 2;
                if (central->pair_result == 0) {
                    mock_note_paired(central->pair_address);
                }
            }
        }
    }
    if (central->pair_state != 2) {
        return 0;
    }
    *out_result = central->pair_result;
    central->pair_state = 0;
    central->pair_token = 0U;
    return 1;
}

void inkwell_ble_pair_cancel(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return;
    }
    (void)inkwell_ble_agent_reject(central);
    if (!mocked() && central->pair_state == 1 && central->pair_address[0] != '\0' &&
        central->backend != NULL) {
        inkwell_ble_backend_pair_cancel(central);
    }
    central->pair_state = 0;
    central->pair_token = 0U;
    central->pair_result = 0;
    central->pair_address[0] = '\0';
}

int inkwell_ble_set_trusted(struct inkwell_ble_central *central, const char *address,
                            bool trusted) {
    if (central == NULL || address == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_set_trusted(central, address, trusted);
}

int inkwell_ble_forget(struct inkwell_ble_central *central, const char *address) {
    if (central == NULL || address == NULL) {
        return -EINVAL;
    }
    if (mocked()) {
        if (g_mock.config.forget_calls != NULL) {
            ++*g_mock.config.forget_calls;
        }
        return g_mock.config.forget_result;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_forget(central, address);
}

/* ---- the pairing agent --------------------------------------------------------------------- */

int inkwell_ble_agent_register(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (central->agent_registered) {
        return 0;
    }
    if (mocked()) {
        central->agent_registered = true;
        if (g_mock.config.agent_register_calls != NULL) {
            (*g_mock.config.agent_register_calls)++;
        }
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    const int result = inkwell_ble_backend_agent_register(central);
    if (result == 0) {
        central->agent_registered = true;
    }
    return result;
}

void inkwell_ble_agent_unregister(struct inkwell_ble_central *central) {
    if (central == NULL || !central->agent_registered) {
        return;
    }
    (void)inkwell_ble_agent_reject(central);
    central->agent_registered = false;
    if (!mocked() && central->backend != NULL) {
        inkwell_ble_backend_agent_unregister(central);
    }
}

bool inkwell_ble_agent_request(const struct inkwell_ble_central *central,
                               struct inkwell_ble_agent_request *out) {
    if (central == NULL || central->agent_request.kind == INKWELL_BLE_AGENT_REQUEST_NONE) {
        return false;
    }
    if (out != NULL) {
        *out = central->agent_request;
    }
    return true;
}

int inkwell_ble_agent_submit_passkey(struct inkwell_ble_central *central, uint32_t passkey) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (central->agent_request.kind == INKWELL_BLE_AGENT_REQUEST_NONE) {
        return -ENOENT;
    }
    if (mocked()) {
        if (g_mock.config.pair_passkey_capture != NULL) {
            *g_mock.config.pair_passkey_capture = passkey;
        }
        memset(&central->agent_request, 0, sizeof central->agent_request);
        return 0;
    }
    const int result = central->backend != NULL
                           ? inkwell_ble_backend_agent_answer(central, true, passkey)
                           : -ENOENT;
    if (result == 0 || result == -ENOENT) {
        memset(&central->agent_request, 0, sizeof central->agent_request);
    }
    return result;
}

int inkwell_ble_agent_confirm(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (central->agent_request.kind != INKWELL_BLE_AGENT_REQUEST_CONFIRM) {
        return -ENOENT;
    }
    return inkwell_ble_agent_submit_passkey(central, central->agent_request.passkey);
}

int inkwell_ble_agent_reject(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (central->agent_request.kind == INKWELL_BLE_AGENT_REQUEST_NONE) {
        return -ENOENT;
    }
    if (mocked()) {
        /* Refusing the question is refusing the bond, which is what a stack does with it. */
        if (central->pair_state == 1) {
            central->pair_result = -EACCES;
        }
    } else if (central->backend != NULL) {
        (void)inkwell_ble_backend_agent_answer(central, false, 0U);
    }
    memset(&central->agent_request, 0, sizeof central->agent_request);
    return 0;
}

/* ---- link state ---------------------------------------------------------------------------- */

static int query(struct inkwell_ble_central *central, const char *address, size_t which,
                 bool *out_value) {
    if (!central->open) {
        return -ENOTCONN;
    }
    struct inkwell_ble_pending *request = &central->requests[which];
    if (request->state != 0) {
        const int result = pending_take(request);
        if (result == 0) {
            *out_value = request->value;
        }
        return result;
    }
    int result = pending_start(central, request, INKWELL_BLE_PROPERTY_TIMEOUT_MS);
    if (result < 0) {
        return result;
    }
    result = inkwell_ble_backend_query(central, address, which, &request->token);
    if (result < 0) {
        pending_cancel(request);
        return result;
    }
    return -EAGAIN;
}

int inkwell_ble_services_resolved(struct inkwell_ble_central *central, const char *address,
                                  bool *out_resolved) {
    if (central == NULL || address == NULL || out_resolved == NULL) {
        return -EINVAL;
    }
    *out_resolved = false;
    if (scripted()) {
        if (g_mock.config.services_resolved_result != 0) {
            return g_mock.config.services_resolved_result;
        }
        if (g_mock.services_resolved_timeouts < g_mock.config.services_resolved_timeout_polls) {
            g_mock.services_resolved_timeouts++;
            return -ETIMEDOUT;
        }
        g_mock.services_resolved_polls++;
        *out_resolved =
            g_mock.services_resolved_polls > g_mock.config.services_resolved_after_polls;
        return 0;
    }
    return query(central, address, 1U, out_resolved);
}

int inkwell_ble_device_connected(struct inkwell_ble_central *central, const char *address,
                                 bool *out_connected) {
    if (central == NULL || address == NULL || out_connected == NULL) {
        return -EINVAL;
    }
    *out_connected = false;
    if (scripted()) {
        g_mock.connected_polls++;
        *out_connected = g_mock.config.connected_drops_after_polls == 0U ||
                         g_mock.connected_polls <= g_mock.config.connected_drops_after_polls;
        return 0;
    }
    return query(central, address, 2U, out_connected);
}

/* ---- GATT ---------------------------------------------------------------------------------- */

int inkwell_ble_find_characteristic(struct inkwell_ble_central *central, const char *address,
                                    const char *char_uuid, char *out_handle, size_t out_len) {
    if (central == NULL || address == NULL || char_uuid == NULL || out_handle == NULL ||
        out_len == 0U) {
        return -EINVAL;
    }
    out_handle[0] = '\0';
    if (scripted()) {
        snprintf(out_handle, out_len, "%s/%s", address, char_uuid);
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_find_characteristic(central, address, char_uuid, out_handle,
                                                   out_len);
}

int inkwell_ble_characteristic_mtu(struct inkwell_ble_central *central, const char *handle,
                                   uint16_t *out_mtu) {
    if (central == NULL || handle == NULL || out_mtu == NULL) {
        return -EINVAL;
    }
    *out_mtu = 0U;
    if (mocked()) {
        if (g_mock.config.mtu == 0U) {
            return -ENOTSUP;
        }
        *out_mtu = g_mock.config.mtu;
        return 0;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    return inkwell_ble_backend_mtu(central, handle, out_mtu);
}

void inkwell_ble_subscribe_finish(struct inkwell_ble_central *central, int result) {
    struct inkwell_ble_pending *request = &central->requests[3];
    if (request->state != 1) {
        return;
    }
    /* Before the caller hears of it: a notification can follow the confirmation at once. */
    if (result == 0) {
        inkwell_str_copy(central->notify_handle, sizeof central->notify_handle,
                         central->subscribe_handle);
    }
    inkwell_ble_pending_finish(request, result);
}

static int mock_subscribe(struct inkwell_ble_central *central, const char *handle) {
    if (!g_mock.subscribing) {
        g_mock.subscribing = true;
        g_mock.subscribe_polls = 0U;
    }
    if (g_mock.subscribe_polls++ < g_mock.config.subscribe_pending_polls) {
        return -EAGAIN;
    }
    g_mock.subscribing = false;
    const int result = g_mock.config.subscribe_result;
    if (result == 0) {
        inkwell_str_copy(central->notify_handle, sizeof central->notify_handle, handle);
    }
    return result;
}

int inkwell_ble_subscribe(struct inkwell_ble_central *central, const char *handle) {
    if (central == NULL || handle == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        return mock_subscribe(central, handle);
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    struct inkwell_ble_pending *request = &central->requests[3];
    if (request->state != 0) {
        return pending_take(request);
    }
    int result = pending_start(central, request, inkwell_ble_backend_subscribe_timeout_ms);
    if (result < 0) {
        return result;
    }
    inkwell_str_copy(central->subscribe_handle, sizeof central->subscribe_handle, handle);
    result = inkwell_ble_backend_subscribe(central, handle, &request->token);
    if (result < 0) {
        pending_cancel(request);
        return result;
    }
    return -EAGAIN;
}

int inkwell_ble_write(struct inkwell_ble_central *central, const char *handle, const uint8_t *data,
                      size_t len) {
    if (central == NULL || handle == NULL || data == NULL) {
        return -EINVAL;
    }
    if (scripted()) {
        return mock_write(handle, data, len);
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    struct inkwell_ble_pending *request = &central->requests[0];
    if (request->state != 0) {
        return pending_take(request);
    }
    int result = pending_start(central, request, inkwell_ble_backend_write_timeout_ms);
    if (result < 0) {
        return result;
    }
    result = inkwell_ble_backend_write(central, handle, data, len, &request->token);
    if (result < 0) {
        pending_cancel(request);
        return result;
    }
    return -EAGAIN;
}

int inkwell_ble_read(struct inkwell_ble_central *central, const char *handle, uint8_t *out,
                     size_t capacity, size_t *out_len) {
    if (central == NULL || handle == NULL || out == NULL || out_len == NULL) {
        return -EINVAL;
    }
    const bool mock_read = scripted();
    *out_len = 0U;
    if (central->read_state == 2) {
        int result = central->read_result;
        if (result == 0 && central->read_length > capacity) {
            result = -EMSGSIZE;
        }
        if (result == 0) {
            memcpy(out, central->read_payload, central->read_length);
            *out_len = central->read_length;
        }
        inkwell_ble_read_cancel(central);
        return result;
    }
    if (central->read_state == 1 && (!mock_read || central->read_mock_polls > 0U)) {
        return -EAGAIN;
    }

    if (mock_read) {
        const struct inkwell_ble_mock_config *cfg = &g_mock.config;
        if (cfg->read_pending_polls > 0U && central->read_state == 0) {
            const int result = read_start(central);
            if (result < 0) {
                return result;
            }
            central->read_mock_polls = cfg->read_pending_polls;
            return -EAGAIN;
        }
        inkwell_ble_read_cancel(central);
        if (cfg->read_result != 0) {
            return cfg->read_result;
        }
        size_t index = cfg->read_index != NULL ? *cfg->read_index : g_mock.read_cursor;
        if (cfg->read_payloads != NULL && cfg->read_payload_lengths != NULL &&
            index < cfg->read_payload_count) {
            const size_t len = cfg->read_payload_lengths[index];
            if (len > capacity) {
                return -EMSGSIZE;
            }
            memcpy(out, cfg->read_payloads[index], len);
            *out_len = len;
        }
        index++;
        if (cfg->read_index != NULL) {
            *cfg->read_index = index;
        } else {
            g_mock.read_cursor = index;
        }
        return 0;
    }

    if (!central->open) {
        return -ENOTCONN;
    }
    int result = read_start(central);
    if (result < 0) {
        return result;
    }
    result = inkwell_ble_backend_read(central, handle, &central->read_token);
    if (result < 0) {
        inkwell_ble_read_cancel(central);
        return result;
    }
    return -EAGAIN;
}

/* ---- the loop ------------------------------------------------------------------------------ */

int inkwell_ble_attach_loop(struct inkwell_ble_central *central, struct inkwell_loop *loop) {
    if (central == NULL) {
        return -EINVAL;
    }
    central->loop = loop;
    if (loop != NULL && central->backend != NULL) {
        return inkwell_ble_backend_attach(central);
    }
    return 0;
}

void inkwell_ble_detach_loop(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return;
    }
    inkwell_ble_read_cancel(central);
    inkwell_ble_requests_cancel(central);
    if (central->loop != NULL && central->backend != NULL) {
        inkwell_ble_backend_detach(central);
    }
    central->loop = NULL;
}

int inkwell_ble_process(struct inkwell_ble_central *central) {
    if (central == NULL) {
        return -EINVAL;
    }
    if (!central->open) {
        return -ENOTCONN;
    }
    const uint64_t now = inkwell_time_monotonic_ms();
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(central->requests); ++i) {
        struct inkwell_ble_pending *request = &central->requests[i];
        if (request->state == 1 && now >= request->deadline_ms) {
            inkwell_ble_pending_finish(request, -ETIMEDOUT);
        }
    }
    if (central->read_state == 1 && now >= central->read_deadline_ms) {
        inkwell_ble_read_finish(central, -ETIMEDOUT);
    }
    if (mocked() && central->read_state == 1 && central->read_mock_polls > 0U) {
        if (--central->read_mock_polls == 0U && central->read_ready != NULL) {
            central->read_ready(central->userdata);
        }
    }
    if (scripted() || central->backend == NULL) {
        return 0;
    }
    return inkwell_ble_backend_process(central);
}

void inkwell_ble_set_notification_handler(struct inkwell_ble_central *central,
                                          inkwell_ble_notification_callback callback,
                                          void *userdata) {
    if (central == NULL) {
        return;
    }
    central->notification_callback = callback;
    central->notification_userdata = userdata;
}
