#include "central_internal.h"

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/runtime/loop.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/*
 * The BlueZ backend: org.bluez over the system D-Bus, driven by libdbus with its watches put on
 * the loop.
 *
 * Three rules shape everything here.
 *
 * **Nothing that can take long blocks.** Connect, Pair, ReadValue, WriteValue and the two
 * property queries are sent and their replies matched by serial in process(). A listing or a
 * lookup reads a copy of bluetoothd's object tree that signals keep current, so the blocking
 * GetManagedObjects behind it is made once per bluetoothd, not once per scan. What does still
 * block - that first fetch, StartNotify, the agent registration - is bounded by an explicit
 * timeout of a second or a few, never libdbus' 25 s default.
 *
 * **Messages are popped here, not dispatched.** process() takes every message off the
 * connection itself, so libdbus' object tree never sees one; the pairing agent is dispatched by
 * hand for the same reason.
 *
 * **A device is `<adapter>/dev_AA_BB_CC_DD_EE_FF`.** That is how bluetoothd names the object for
 * an address, and it is the whole of the translation between the caller's vocabulary and this
 * one. A characteristic's handle is simply its object path.
 */

/* Our org.bluez.Agent1 object. BlueZ calls back on this path to ask for a PIN. */
#define INKWELL_BLUEZ_AGENT_PATH "/org/inkwell/agent"
#define INKWELL_BLUEZ_DEFAULT_ADAPTER "/org/bluez/hci0"
#define INKWELL_BLUEZ_WATCHES 8U
/*
 * The bound on the blocking calls that used to take libdbus' 25 s default: discovery on and off,
 * and Disconnect. bluetoothd answers each in milliseconds when it is well; this only caps how
 * long the loop stalls when it is not. Making them asynchronous is the real fix and a larger one:
 * their callers act on the answer in the same turn.
 */
#define INKWELL_BLUEZ_ADAPTER_TIMEOUT_MS 5000

/* A bond is made before a link is used (pair_begin), so a write has nothing to wait behind. */
const unsigned inkwell_ble_backend_write_timeout_ms = 3000U;

struct bluez_watch {
    DBusWatch *watch;
    int fd;
    bool registered;
};

/* 36 characters and the terminator. */
#define INKWELL_BLUEZ_UUID_LEN 37U

/* The interfaces the object tree below keeps. */
enum {
    BLUEZ_ADAPTER = 1U << 0,
    BLUEZ_DEVICE = 1U << 1,
    BLUEZ_CHARACTERISTIC = 1U << 2,
};

struct bluez_object {
    char path[INKWELL_BLE_HANDLE_MAX];
    unsigned interfaces;
    /* Device1 */
    char address[INKWELL_BLE_ADDRESS_MAX];
    char name[INKWELL_BLE_NAME_MAX];
    char alias[INKWELL_BLE_NAME_MAX];
    int16_t rssi;
    bool has_rssi;
    bool paired;
    char (*services)[INKWELL_BLUEZ_UUID_LEN];
    size_t service_count;
    /* GattCharacteristic1 */
    char uuid[INKWELL_BLUEZ_UUID_LEN];
};

struct bluez_backend {
    DBusConnection *connection;
    bool connection_private;
    /* An agent call held until the user answers it. */
    DBusMessage *agent_pending;
    struct bluez_watch watches[INKWELL_BLUEZ_WATCHES];
    /* The object tree; see its section. */
    struct bluez_object *objects;
    size_t object_count;
    size_t object_capacity;
    bool objects_loaded;
    /* A GetManagedObjects sent without waiting, or 0. */
    dbus_uint32_t objects_serial;
};

/* What keeps the object tree current, subscribed to in open(); see that section. */
static const char *const k_object_rules[] = {
    "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
    "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
    "member='PropertiesChanged',arg0='org.bluez.Device1'",
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
    "member='NameOwnerChanged',arg0='org.bluez'",
};

static void objects_clear(struct bluez_backend *backend);

static struct bluez_backend *backend_of(struct inkwell_ble_central *central) {
    return (struct bluez_backend *)central->backend;
}

static DBusConnection *connection_of(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    return backend != NULL ? backend->connection : NULL;
}

/* ---- names ----------------------------------------------------------------------------------- */

/* "FB:17:7C:37:6D:DA" -> "/org/bluez/hci0/dev_FB_17_7C_37_6D_DA". */
static bool device_path(const struct inkwell_ble_central *central, const char *address, char *out,
                        size_t out_len) {
    if (address == NULL || address[0] == '\0') {
        return false;
    }
    const char *adapter =
        central->adapter[0] != '\0' ? central->adapter : INKWELL_BLUEZ_DEFAULT_ADAPTER;
    char mangled[INKWELL_BLE_ADDRESS_MAX];
    size_t i = 0U;
    for (; address[i] != '\0' && i + 1U < sizeof mangled; ++i) {
        mangled[i] = address[i] == ':' ? '_' : address[i];
    }
    mangled[i] = '\0';
    const int written = snprintf(out, out_len, "%s/dev_%s", adapter, mangled);
    return written > 0 && (size_t)written < out_len;
}

/* The inverse, for naming the peripheral an agent request came in for. */
static void address_from_path(const char *path, char *out, size_t out_len) {
    out[0] = '\0';
    const char *dev = path != NULL ? strstr(path, "/dev_") : NULL;
    if (dev == NULL || out_len == 0U) {
        return;
    }
    dev += 5;
    size_t written = 0U;
    while (*dev != '\0' && *dev != '/' && written + 1U < out_len) {
        out[written++] = *dev == '_' ? ':' : *dev;
        dev++;
    }
    out[written] = '\0';
}

/* ---- errors ---------------------------------------------------------------------------------- */

/*
 * Turns a BlueZ D-Bus error into an errno a caller can act on. Everything used to come back as
 * -EIO, which left a caller unable to tell "this peripheral needs pairing" - the one failure a
 * user can actually fix - apart from one that is simply out of range. BlueZ is not consistent
 * about which name it uses (StartNotify on an unbonded peripheral answers
 * org.bluez.Error.Failed with "Not paired"), so the message is checked too.
 */
static int error_to_errno(const char *name, const char *message) {
    if (name != NULL) {
        const char *suffix = strrchr(name, '.');
        suffix = suffix != NULL ? suffix + 1 : name;
        if (strcmp(suffix, "NotPaired") == 0 || strcmp(suffix, "NotPermitted") == 0 ||
            strcmp(suffix, "NotAuthorized") == 0 || strncmp(suffix, "Authentication", 14) == 0) {
            return -EACCES;
        }
        if (strcmp(suffix, "NotConnected") == 0 || strcmp(suffix, "NotReady") == 0) {
            return -ENOTCONN;
        }
        if (strcmp(suffix, "InProgress") == 0) {
            return -EBUSY;
        }
        if (strcmp(suffix, "NoReply") == 0 || strcmp(suffix, "Timeout") == 0 ||
            strcmp(suffix, "TimedOut") == 0) {
            return -ETIMEDOUT;
        }
        if (strcmp(suffix, "DoesNotExist") == 0 || strcmp(suffix, "UnknownObject") == 0) {
            return -ENOENT;
        }
    }
    if (message != NULL) {
        if (strcasecmp(message, "Not paired") == 0 || strcasecmp(message, "Not Authorized") == 0) {
            return -EACCES;
        }
        if (strcasecmp(message, "Page Timeout") == 0 ||
            strcasecmp(message, "Connection Timeout") == 0) {
            return -ETIMEDOUT;
        }
    }
    return -EIO;
}

static int dbus_error_to_errno(const DBusError *error) {
    if (error == NULL || !dbus_error_is_set(error)) {
        return -EIO;
    }
    return error_to_errno(error->name, error->message);
}

/* The errno an error reply stands for, logged as `what` with BlueZ's own words. */
static int reply_error(DBusMessage *reply, const char *what) {
    const char *error_name = dbus_message_get_error_name(reply);
    char *text = NULL;
    dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    inkwell_log_warn("ble", "%s failed: %s%s%s", what, error_name != NULL ? error_name : "?",
                     text != NULL ? ": " : "", text != NULL ? text : "");
    return error_to_errno(error_name, text);
}

/* ---- watches ------------------------------------------------------------------------------- */

static uint32_t watch_flags_to_events(unsigned int flags) {
    uint32_t events = 0U;
    if ((flags & DBUS_WATCH_READABLE) != 0U) {
        events |= INKWELL_LOOP_IN;
    }
    if ((flags & DBUS_WATCH_WRITABLE) != 0U) {
        events |= INKWELL_LOOP_OUT;
    }
    if ((flags & DBUS_WATCH_ERROR) != 0U) {
        events |= INKWELL_LOOP_ERR;
    }
    if ((flags & DBUS_WATCH_HANGUP) != 0U) {
        events |= INKWELL_LOOP_HUP;
    }
    return events;
}

static int watch_fd_callback(int fd, uint32_t events, void *userdata);

static int watch_sync(struct inkwell_ble_central *central, size_t index) {
    struct bluez_backend *backend = backend_of(central);
    struct bluez_watch *entry = &backend->watches[index];
    if (entry->watch == NULL) {
        return 0;
    }
    entry->fd = dbus_watch_get_unix_fd(entry->watch);
    if (central->loop == NULL) {
        return 0;
    }

    /* libdbus has separate readable and writable watches for one socket, and the loop has one
       registration per descriptor: combine the enabled watches, or queued nonblocking writes
       can lose INKWELL_LOOP_OUT behind an EEXIST from the readable one. */
    uint32_t events = 0U;
    struct bluez_watch *owner = NULL;
    for (size_t i = 0U; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        struct bluez_watch *other = &backend->watches[i];
        if (other->watch == NULL || dbus_watch_get_unix_fd(other->watch) != entry->fd) {
            continue;
        }
        if (other->registered) {
            owner = other;
        }
        if (dbus_watch_get_enabled(other->watch)) {
            events |= watch_flags_to_events(dbus_watch_get_flags(other->watch));
        }
    }
    if (owner != NULL) {
        if (events == 0U) {
            inkwell_loop_remove_fd(central->loop, entry->fd);
            owner->registered = false;
            return 0;
        }
        return inkwell_loop_update_fd(central->loop, entry->fd, events);
    }
    if (events == 0U) {
        return 0;
    }
    const int result =
        inkwell_loop_add_fd(central->loop, entry->fd, events, watch_fd_callback, central);
    if (result < 0) {
        inkwell_log_warn("ble", "Failed to register D-Bus watch fd %d: %d", entry->fd, result);
        return result;
    }
    entry->registered = true;
    return 0;
}

static ssize_t watch_find(struct bluez_backend *backend, DBusWatch *watch) {
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        if (backend->watches[i].watch == watch) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static dbus_bool_t watch_add(DBusWatch *watch, void *userdata) {
    struct inkwell_ble_central *central = userdata;
    struct bluez_backend *backend = backend_of(central);
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        if (backend->watches[i].watch == NULL) {
            backend->watches[i].watch = watch;
            backend->watches[i].registered = false;
            (void)watch_sync(central, i);
            return TRUE;
        }
    }
    inkwell_log_warn("ble", "No space for additional D-Bus watches");
    return FALSE;
}

static void watch_remove(DBusWatch *watch, void *userdata) {
    struct inkwell_ble_central *central = userdata;
    struct bluez_backend *backend = backend_of(central);
    const ssize_t index = watch_find(backend, watch);
    if (index < 0) {
        return;
    }
    struct bluez_watch *entry = &backend->watches[index];
    const int fd = dbus_watch_get_unix_fd(watch);
    if (entry->registered && central->loop != NULL) {
        inkwell_loop_remove_fd(central->loop, entry->fd);
    }
    entry->watch = NULL;
    entry->fd = -1;
    entry->registered = false;
    /* Another watch on the same socket takes over the registration. */
    for (size_t i = 0U; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        if (backend->watches[i].watch != NULL &&
            dbus_watch_get_unix_fd(backend->watches[i].watch) == fd) {
            (void)watch_sync(central, i);
            break;
        }
    }
}

static void watch_toggled(DBusWatch *watch, void *userdata) {
    struct inkwell_ble_central *central = userdata;
    const ssize_t index = watch_find(backend_of(central), watch);
    if (index >= 0) {
        (void)watch_sync(central, (size_t)index);
    }
}

static int watch_fd_callback(int fd, uint32_t events, void *userdata) {
    struct inkwell_ble_central *central = userdata;
    struct bluez_backend *backend = backend_of(central);
    if (backend == NULL) {
        return 0;
    }
    unsigned int flags = 0U;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        flags |= DBUS_WATCH_READABLE;
    }
    if ((events & INKWELL_LOOP_OUT) != 0U) {
        flags |= DBUS_WATCH_WRITABLE;
    }
    if ((events & INKWELL_LOOP_ERR) != 0U) {
        flags |= DBUS_WATCH_ERROR;
    }
    if ((events & INKWELL_LOOP_HUP) != 0U) {
        flags |= DBUS_WATCH_HANGUP;
    }
    for (size_t i = 0U; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        DBusWatch *watch = backend->watches[i].watch;
        if (watch == NULL || dbus_watch_get_unix_fd(watch) != fd ||
            !dbus_watch_get_enabled(watch)) {
            continue;
        }
        const unsigned int relevant =
            flags & (dbus_watch_get_flags(watch) | DBUS_WATCH_ERROR | DBUS_WATCH_HANGUP);
        if (relevant != 0U && !dbus_watch_handle(watch, relevant)) {
            inkwell_log_warn("ble", "dbus_watch_handle returned false");
        }
    }
    (void)inkwell_ble_process(central);
    return 0;
}

/* ---- lifecycle ----------------------------------------------------------------------------- */

int inkwell_ble_backend_open(struct inkwell_ble_central *central, bool private_connection,
                             const char *bus_address) {
    struct bluez_backend *backend = calloc(1U, sizeof *backend);
    if (backend == NULL) {
        return -ENOMEM;
    }
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        backend->watches[i].fd = -1;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *connection = NULL;
    if (bus_address != NULL) {
        connection = dbus_connection_open_private(bus_address, &error);
        if (connection != NULL && !dbus_bus_register(connection, &error)) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            connection = NULL;
        }
    } else if (private_connection) {
        connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    } else {
        connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    }
    if (connection == NULL) {
        if (dbus_error_is_set(&error)) {
            inkwell_log_warn("ble", "Failed to connect to system bus: %s", error.message);
            dbus_error_free(&error);
        }
        free(backend);
        return -EIO;
    }
    backend->connection = connection;
    backend->connection_private = bus_address != NULL || private_connection;
    dbus_connection_set_exit_on_disconnect(connection, false);

    /* Set before the watch functions: libdbus calls watch_add() from inside the setter. */
    central->backend = backend;
    if (!dbus_connection_set_watch_functions(connection, watch_add, watch_remove, watch_toggled,
                                             central, NULL)) {
        central->backend = NULL;
        if (backend->connection_private) {
            dbus_connection_close(connection);
        }
        dbus_connection_unref(connection);
        free(backend);
        return -EIO;
    }
    /* Without an error to fill, AddMatch is sent and not waited for. */
    for (size_t i = 0U; i < INKWELL_ARRAY_LEN(k_object_rules); ++i) {
        dbus_bus_add_match(connection, k_object_rules[i], NULL);
    }
    dbus_connection_flush(connection);
    return 0;
}

void inkwell_ble_backend_close(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    if (backend == NULL) {
        return;
    }
    if (backend->agent_pending != NULL) {
        dbus_message_unref(backend->agent_pending);
        backend->agent_pending = NULL;
    }
    if (backend->connection != NULL) {
        dbus_connection_set_watch_functions(backend->connection, NULL, NULL, NULL, NULL, NULL);
        if (backend->connection_private) {
            dbus_connection_close(backend->connection);
        }
        dbus_connection_unref(backend->connection);
    }
    objects_clear(backend);
    free(backend->objects);
    free(backend);
    central->backend = NULL;
}

int inkwell_ble_backend_check_ready(struct inkwell_ble_central *central) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusError error;
    dbus_error_init(&error);
    const dbus_bool_t has_owner = dbus_bus_name_has_owner(connection, "org.bluez", &error);
    if (dbus_error_is_set(&error)) {
        inkwell_log_warn("ble", "Failed to query BlueZ ownership: %s", error.message);
        dbus_error_free(&error);
        return -EIO;
    }
    return has_owner ? 0 : -ENODEV;
}

int inkwell_ble_backend_attach(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        if (backend->watches[i].watch != NULL) {
            (void)watch_sync(central, i);
        }
    }
    return 0;
}

void inkwell_ble_backend_detach(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(backend->watches); ++i) {
        if (backend->watches[i].registered) {
            inkwell_loop_remove_fd(central->loop, backend->watches[i].fd);
        }
        backend->watches[i].registered = false;
    }
}

/* ---- the object tree ------------------------------------------------------------------------ *
 *
 * A copy of the part of bluetoothd's object tree this backend asks about - adapters, devices and
 * characteristics - kept current from the signals BlueZ sends as the tree changes. The tree used
 * to be fetched whole, with a blocking GetManagedObjects, on every listing: once a second for as
 * long as a caller scans. Now it is fetched once, the first time it is needed, and again without
 * blocking whenever bluetoothd restarts.
 *
 * The signals are subscribed to in open(), before any fetch, so nothing that changes after a
 * snapshot is missed. A signal that is still queued when a blocking fetch returns is older than
 * the snapshot and is applied on top of it, which converges: the last word on any object or
 * property is still the newest one. While no snapshot is held, signals are ignored - the one
 * that is coming is newer than all of them.
 */

static unsigned interface_bit(const char *name) {
    if (name == NULL) {
        return 0U;
    }
    if (strcmp(name, "org.bluez.Adapter1") == 0) {
        return BLUEZ_ADAPTER;
    }
    if (strcmp(name, "org.bluez.Device1") == 0) {
        return BLUEZ_DEVICE;
    }
    if (strcmp(name, "org.bluez.GattCharacteristic1") == 0) {
        return BLUEZ_CHARACTERISTIC;
    }
    return 0U;
}

static struct bluez_object *object_find(struct bluez_backend *backend, const char *path) {
    for (size_t i = 0U; i < backend->object_count; ++i) {
        if (strcmp(backend->objects[i].path, path) == 0) {
            return &backend->objects[i];
        }
    }
    return NULL;
}

/* The object at `path`, added if it is not there yet. NULL when it cannot be kept. */
static struct bluez_object *object_add(struct bluez_backend *backend, const char *path) {
    struct bluez_object *object = object_find(backend, path);
    if (object != NULL) {
        return object;
    }
    if (strlen(path) >= sizeof object->path) {
        return NULL;
    }
    if (backend->object_count == backend->object_capacity) {
        const size_t capacity =
            backend->object_capacity == 0U ? 32U : backend->object_capacity * 2U;
        struct bluez_object *grown = realloc(backend->objects, capacity * sizeof *grown);
        if (grown == NULL) {
            inkwell_log_warn("ble", "No memory to track %s", path);
            return NULL;
        }
        backend->objects = grown;
        backend->object_capacity = capacity;
    }
    object = &backend->objects[backend->object_count++];
    memset(object, 0, sizeof *object);
    inkwell_str_copy(object->path, sizeof object->path, path);
    return object;
}

static void object_clear_services(struct bluez_object *object) {
    free(object->services);
    object->services = NULL;
    object->service_count = 0U;
}

/* Order is kept, so a listing comes back in the order bluetoothd first reported each device. */
static void object_remove(struct bluez_backend *backend, struct bluez_object *object) {
    object_clear_services(object);
    const size_t index = (size_t)(object - backend->objects);
    memmove(object, object + 1, (backend->object_count - index - 1U) * sizeof *object);
    --backend->object_count;
}

static void objects_clear(struct bluez_backend *backend) {
    for (size_t i = 0U; i < backend->object_count; ++i) {
        object_clear_services(&backend->objects[i]);
    }
    backend->object_count = 0U;
    backend->objects_loaded = false;
}

/* UUIDs is replaced whole, never patched. */
static void object_set_services(struct bluez_object *object, DBusMessageIter *array) {
    object_clear_services(object);
    DBusMessageIter uuids;
    dbus_message_iter_recurse(array, &uuids);
    size_t count = 0U;
    for (DBusMessageIter it = uuids; dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING;
         dbus_message_iter_next(&it)) {
        ++count;
    }
    if (count == 0U) {
        return;
    }
    object->services = calloc(count, sizeof *object->services);
    if (object->services == NULL) {
        inkwell_log_warn("ble", "No memory for the services of %s", object->path);
        return;
    }
    for (; dbus_message_iter_get_arg_type(&uuids) == DBUS_TYPE_STRING;
         dbus_message_iter_next(&uuids)) {
        const char *uuid = NULL;
        dbus_message_iter_get_basic(&uuids, &uuid);
        inkwell_str_copy(object->services[object->service_count++], sizeof object->services[0],
                         uuid != NULL ? uuid : "");
    }
}

static void object_set_property(struct bluez_object *object, unsigned interface, const char *name,
                                DBusMessageIter *variant) {
    const int type = dbus_message_iter_get_arg_type(variant);
    const char *text = NULL;
    if (type == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(variant, &text);
    }
    if (interface == BLUEZ_CHARACTERISTIC) {
        if (strcmp(name, "UUID") == 0 && text != NULL) {
            inkwell_str_copy(object->uuid, sizeof object->uuid, text);
        }
        return;
    }
    if (interface != BLUEZ_DEVICE) {
        return;
    }
    if (strcmp(name, "Address") == 0 && text != NULL) {
        inkwell_str_copy(object->address, sizeof object->address, text);
    } else if (strcmp(name, "Name") == 0 && text != NULL) {
        inkwell_str_copy(object->name, sizeof object->name, text);
    } else if (strcmp(name, "Alias") == 0 && text != NULL) {
        inkwell_str_copy(object->alias, sizeof object->alias, text);
    } else if (strcmp(name, "Paired") == 0 && type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t paired = FALSE;
        dbus_message_iter_get_basic(variant, &paired);
        object->paired = paired != FALSE;
    } else if (strcmp(name, "RSSI") == 0 && type == DBUS_TYPE_INT16) {
        int16_t rssi = 0;
        dbus_message_iter_get_basic(variant, &rssi);
        object->rssi = rssi;
        object->has_rssi = true;
    } else if (strcmp(name, "UUIDs") == 0 && type == DBUS_TYPE_ARRAY) {
        object_set_services(object, variant);
    }
}

/* A property that has stopped existing. RSSI is the one that matters: bluetoothd drops it when a
   device goes unheard, and its absence is what `in_range` reports. */
static void object_unset_property(struct bluez_object *object, unsigned interface,
                                  const char *name) {
    if (interface != BLUEZ_DEVICE) {
        return;
    }
    if (strcmp(name, "RSSI") == 0) {
        object->has_rssi = false;
        object->rssi = 0;
    } else if (strcmp(name, "Name") == 0) {
        object->name[0] = '\0';
    } else if (strcmp(name, "Alias") == 0) {
        object->alias[0] = '\0';
    } else if (strcmp(name, "Paired") == 0) {
        object->paired = false;
    } else if (strcmp(name, "UUIDs") == 0) {
        object_clear_services(object);
    }
}

/* An a{sv}: some or all of `interface`'s properties. */
static void object_set_properties(struct bluez_object *object, unsigned interface,
                                  DBusMessageIter *array) {
    DBusMessageIter properties;
    dbus_message_iter_recurse(array, &properties);
    for (; dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY;
         dbus_message_iter_next(&properties)) {
        DBusMessageIter property;
        dbus_message_iter_recurse(&properties, &property);
        if (dbus_message_iter_get_arg_type(&property) != DBUS_TYPE_STRING) {
            continue;
        }
        const char *name = NULL;
        dbus_message_iter_get_basic(&property, &name);
        if (!dbus_message_iter_next(&property) ||
            dbus_message_iter_get_arg_type(&property) != DBUS_TYPE_VARIANT) {
            continue;
        }
        DBusMessageIter variant;
        dbus_message_iter_recurse(&property, &variant);
        object_set_property(object, interface, name, &variant);
    }
}

/* An a{sa{sv}}: the interfaces an object has, as GetManagedObjects and InterfacesAdded list
   them. An object with none this copy keeps is never added. */
static void objects_add_interfaces(struct bluez_backend *backend, const char *path,
                                   DBusMessageIter *array) {
    DBusMessageIter interfaces;
    dbus_message_iter_recurse(array, &interfaces);
    for (; dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY;
         dbus_message_iter_next(&interfaces)) {
        DBusMessageIter interface;
        dbus_message_iter_recurse(&interfaces, &interface);
        const char *name = NULL;
        if (dbus_message_iter_get_arg_type(&interface) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&interface, &name);
        }
        const unsigned bit = interface_bit(name);
        if (bit == 0U || !dbus_message_iter_next(&interface) ||
            dbus_message_iter_get_arg_type(&interface) != DBUS_TYPE_ARRAY) {
            continue;
        }
        struct bluez_object *object = object_add(backend, path);
        if (object == NULL) {
            return;
        }
        object->interfaces |= bit;
        object_set_properties(object, bit, &interface);
    }
}

/* An `as`: the interfaces an object has lost. It goes once it has none this copy keeps. */
static void objects_remove_interfaces(struct bluez_backend *backend, const char *path,
                                      DBusMessageIter *array) {
    struct bluez_object *object = object_find(backend, path);
    if (object == NULL) {
        return;
    }
    DBusMessageIter names;
    dbus_message_iter_recurse(array, &names);
    for (; dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING;
         dbus_message_iter_next(&names)) {
        const char *name = NULL;
        dbus_message_iter_get_basic(&names, &name);
        object->interfaces &= ~interface_bit(name);
    }
    if (object->interfaces == 0U) {
        object_remove(backend, object);
    }
}

/* Replaces the whole copy with a GetManagedObjects reply. */
static int objects_load(struct bluez_backend *backend, DBusMessage *reply) {
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        return reply_error(reply, "GetManagedObjects");
    }
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        return -EPROTO;
    }
    objects_clear(backend);
    DBusMessageIter objects;
    dbus_message_iter_recurse(&iter, &objects);
    for (; dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY;
         dbus_message_iter_next(&objects)) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&objects, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_OBJECT_PATH) {
            continue;
        }
        const char *path = NULL;
        dbus_message_iter_get_basic(&entry, &path);
        if (dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            objects_add_interfaces(backend, path, &entry);
        }
    }
    backend->objects_loaded = true;
    return 0;
}

static DBusMessage *managed_objects_call(void) {
    return dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                        "GetManagedObjects");
}

/* A fresh snapshot, fetched without waiting: process() loads the reply. */
static void objects_request(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    backend->objects_serial = 0U;
    DBusMessage *message = managed_objects_call();
    if (message == NULL) {
        return;
    }
    dbus_uint32_t serial = 0U;
    if (dbus_connection_send(backend->connection, message, &serial)) {
        backend->objects_serial = serial;
    }
    dbus_message_unref(message);
}

/* The copy, fetched now if there is none: the one blocking GetManagedObjects, bounded to a
   second, and made once per bluetoothd rather than once per listing. */
static int objects_ensure(struct inkwell_ble_central *central) {
    struct bluez_backend *backend = backend_of(central);
    if (backend->objects_loaded) {
        return 0;
    }
    DBusMessage *message = managed_objects_call();
    if (message == NULL) {
        return -ENOMEM;
    }
    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(backend->connection, message, 1000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            inkwell_log_warn("ble", "GetManagedObjects failed: %s", error.message);
            dbus_error_free(&error);
        }
        return -EIO;
    }
    /* Anything still in flight is older than this. */
    backend->objects_serial = 0U;
    const int result = objects_load(backend, reply);
    dbus_message_unref(reply);
    return result < 0 ? -EIO : 0;
}

/* The signals that keep the copy current. Returns true when `message` was one of them. */
static bool objects_handle_signal(struct inkwell_ble_central *central, DBusMessage *message) {
    struct bluez_backend *backend = backend_of(central);
    if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = NULL;
        const char *old_owner = NULL;
        const char *new_owner = NULL;
        if (!dbus_message_has_sender(message, "org.freedesktop.DBus") ||
            !dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING,
                                   &old_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) ||
            strcmp(name, "org.bluez") != 0) {
            return true;
        }
        /* A restarted bluetoothd has a new tree and none of the old one's objects. */
        objects_clear(backend);
        backend->objects_serial = 0U;
        if (new_owner[0] != '\0') {
            objects_request(central);
        }
        return true;
    }
    const bool added =
        dbus_message_is_signal(message, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    const bool removed =
        !added &&
        dbus_message_is_signal(message, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved");
    if (added || removed) {
        DBusMessageIter iter;
        const char *path = NULL;
        if (backend->objects_loaded && dbus_message_iter_init(message, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &path);
            if (dbus_message_iter_next(&iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
                if (added) {
                    objects_add_interfaces(backend, path, &iter);
                } else {
                    objects_remove_interfaces(backend, path, &iter);
                }
            }
        }
        return true;
    }
    return false;
}

/* A Device1 PropertiesChanged: `iter` is on the interface name. */
static void objects_properties_changed(struct bluez_backend *backend, const char *path,
                                       DBusMessageIter *iter) {
    struct bluez_object *object =
        backend->objects_loaded && path != NULL ? object_find(backend, path) : NULL;
    if (object == NULL || (object->interfaces & BLUEZ_DEVICE) == 0U ||
        !dbus_message_iter_next(iter) || dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
        return;
    }
    object_set_properties(object, BLUEZ_DEVICE, iter);
    if (!dbus_message_iter_next(iter) || dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter invalidated;
    dbus_message_iter_recurse(iter, &invalidated);
    for (; dbus_message_iter_get_arg_type(&invalidated) == DBUS_TYPE_STRING;
         dbus_message_iter_next(&invalidated)) {
        const char *name = NULL;
        dbus_message_iter_get_basic(&invalidated, &name);
        object_unset_property(object, BLUEZ_DEVICE, name);
    }
}

int inkwell_ble_backend_find_adapter(struct inkwell_ble_central *central, char *name,
                                     size_t name_len) {
    if (connection_of(central) == NULL) {
        return -ENOTCONN;
    }
    const int loaded = objects_ensure(central);
    if (loaded < 0) {
        return loaded;
    }
    const struct bluez_backend *backend = backend_of(central);
    for (size_t i = 0U; i < backend->object_count; ++i) {
        const struct bluez_object *object = &backend->objects[i];
        if ((object->interfaces & BLUEZ_ADAPTER) != 0U) {
            inkwell_str_copy(central->adapter, sizeof central->adapter, object->path);
            inkwell_str_copy(name, name_len, object->path);
            return 0;
        }
    }
    return -ENODEV;
}

static bool object_has_service(const struct bluez_object *object, const char *uuid) {
    for (size_t i = 0U; i < object->service_count; ++i) {
        if (strcasecmp(object->services[i], uuid) == 0) {
            return true;
        }
    }
    return false;
}

int inkwell_ble_backend_list_by_service(struct inkwell_ble_central *central,
                                        const char *service_uuid,
                                        struct inkwell_ble_device *devices, size_t capacity,
                                        size_t *count) {
    if (connection_of(central) == NULL) {
        return -ENOTCONN;
    }
    const int loaded = objects_ensure(central);
    if (loaded < 0) {
        return loaded;
    }
    const struct bluez_backend *backend = backend_of(central);
    size_t matched = 0U;
    for (size_t i = 0U; i < backend->object_count; ++i) {
        const struct bluez_object *object = &backend->objects[i];
        if ((object->interfaces & BLUEZ_DEVICE) == 0U ||
            !object_has_service(object, service_uuid)) {
            continue;
        }
        if (matched < capacity) {
            struct inkwell_ble_device *info = &devices[matched];
            memset(info, 0, sizeof *info);
            inkwell_str_copy(info->address, sizeof info->address, object->address);
            inkwell_str_copy(info->name, sizeof info->name,
                             object->name[0] != '\0' ? object->name : object->alias);
            info->paired = object->paired;
            info->rssi = object->rssi;
            /* The property being there at all is the range test; see `in_range`. */
            info->in_range = object->has_rssi;
        } else {
            inkwell_log_warn("ble", "Device list full, dropping entry");
        }
        ++matched;
    }
    *count = matched > capacity ? capacity : matched;
    return 0;
}

int inkwell_ble_backend_find_characteristic(struct inkwell_ble_central *central,
                                            const char *address, const char *char_uuid,
                                            char *out_handle, size_t out_len) {
    if (connection_of(central) == NULL) {
        return -ENOTCONN;
    }
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    const int loaded = objects_ensure(central);
    if (loaded < 0) {
        return loaded;
    }
    /* The trailing slash keeps dev_..._0A from matching dev_..._0AB's children. */
    char prefix[INKWELL_BLE_HANDLE_MAX + 1U];
    snprintf(prefix, sizeof prefix, "%s/", path);
    const size_t prefix_len = strlen(prefix);
    const struct bluez_backend *backend = backend_of(central);
    for (size_t i = 0U; i < backend->object_count; ++i) {
        const struct bluez_object *object = &backend->objects[i];
        if ((object->interfaces & BLUEZ_CHARACTERISTIC) != 0U &&
            strncmp(object->path, prefix, prefix_len) == 0 &&
            strcasecmp(object->uuid, char_uuid) == 0) {
            inkwell_str_copy(out_handle, out_len, object->path);
            return 0;
        }
    }
    return -ENOENT;
}

/* ---- method calls ---------------------------------------------------------------------------- */

/* One call with no arguments, sent without waiting; its serial goes to *token. */
static int send_call(struct inkwell_ble_central *central, const char *path, const char *interface,
                     const char *method, uint32_t *token) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message = dbus_message_new_method_call("org.bluez", path, interface, method);
    if (message == NULL) {
        return -ENOMEM;
    }
    dbus_uint32_t serial = 0U;
    const dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -EIO;
    }
    dbus_connection_flush(connection);
    if (token != NULL) {
        *token = serial;
    }
    return 0;
}

/* One call answered within `timeout_ms`, which the loop waits out: only for calls that are
   quick, rare, or both. */
static int call_blocking(struct inkwell_ble_central *central, DBusMessage *message, int timeout_ms,
                         const char *what) {
    DBusConnection *connection = connection_of(central);
    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, timeout_ms, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            inkwell_log_warn("ble", "%s failed: %s", what, error.message);
            mapped = dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }
    dbus_message_unref(reply);
    return 0;
}

int inkwell_ble_backend_discovery(struct inkwell_ble_central *central, bool on) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    const char *method = on ? "StartDiscovery" : "StopDiscovery";
    const char *adapter =
        central->adapter[0] != '\0' ? central->adapter : INKWELL_BLUEZ_DEFAULT_ADAPTER;
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", adapter, "org.bluez.Adapter1", method);
    if (message == NULL) {
        return -ENOMEM;
    }
    const int result = call_blocking(central, message, INKWELL_BLUEZ_ADAPTER_TIMEOUT_MS, method);
    /* Adapter errors were always reported as -EIO, and callers log the errno as a reason. */
    return result < 0 ? -EIO : 0;
}

int inkwell_ble_backend_connect(struct inkwell_ble_central *central, const char *address,
                                uint32_t *token) {
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    return send_call(central, path, "org.bluez.Device1", "Connect", token);
}

void inkwell_ble_backend_connect_cancel(struct inkwell_ble_central *central) {
    /* Nothing to send: bluetoothd has no CancelConnect, and the late reply is dropped by its
       token having been cleared. */
    (void)central;
}

int inkwell_ble_backend_disconnect(struct inkwell_ble_central *central, const char *address) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", path, "org.bluez.Device1", "Disconnect");
    if (message == NULL) {
        return -ENOMEM;
    }
    if (call_blocking(central, message, INKWELL_BLUEZ_ADAPTER_TIMEOUT_MS, "Disconnect") < 0) {
        return -EIO;
    }
    if (central->notify_handle[0] != '\0') {
        char rule[256];
        snprintf(rule, sizeof rule,
                 "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
                 "member='PropertiesChanged',path='%s'",
                 central->notify_handle);
        dbus_bus_remove_match(connection, rule, NULL);
        dbus_connection_flush(connection);
    }
    return 0;
}

/* ---- pairing ------------------------------------------------------------------------------- *
 *
 * Everything a peripheral with a PIN on its screen needs, so the user never has to leave the
 * application for bluetoothctl. Two halves that only make sense together: Device1.Pair, sent
 * without blocking because it does not answer until the bond is done, and an org.bluez.Agent1
 * that BlueZ calls back on to ask for the six digits. The agent's reply is deferred - the call is
 * held until the user has typed them - which is why a pair can outlive many turns of the loop.
 */

int inkwell_ble_backend_pair(struct inkwell_ble_central *central, const char *address,
                             uint32_t *token) {
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    return send_call(central, path, "org.bluez.Device1", "Pair", token);
}

void inkwell_ble_backend_pair_cancel(struct inkwell_ble_central *central) {
    char path[INKWELL_BLE_HANDLE_MAX];
    if (device_path(central, central->pair_address, path, sizeof path)) {
        (void)send_call(central, path, "org.bluez.Device1", "CancelPairing", NULL);
    }
}

int inkwell_ble_backend_set_trusted(struct inkwell_ble_central *central, const char *address,
                                    bool trusted) {
    if (connection_of(central) == NULL) {
        return -ENOTCONN;
    }
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", path, "org.freedesktop.DBus.Properties", "Set");
    if (message == NULL) {
        return -ENOMEM;
    }
    const char *interface = "org.bluez.Device1";
    const char *property = "Trusted";
    dbus_bool_t value = trusted ? TRUE : FALSE;
    DBusMessageIter iter;
    DBusMessageIter variant;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING,
                                     &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&iter, &variant);
    return call_blocking(central, message, 2000, "Set Trusted");
}

int inkwell_ble_backend_forget(struct inkwell_ble_central *central, const char *address) {
    if (connection_of(central) == NULL) {
        return -ENOTCONN;
    }
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    const char *adapter =
        central->adapter[0] != '\0' ? central->adapter : INKWELL_BLUEZ_DEFAULT_ADAPTER;
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", adapter, "org.bluez.Adapter1", "RemoveDevice");
    if (message == NULL) {
        return -ENOMEM;
    }
    const char *object = path;
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &object, DBUS_TYPE_INVALID);
    const int result = call_blocking(central, message, 5000, "RemoveDevice");
    if (result == 0) {
        inkwell_log_info("ble", "Removed %s", path);
    }
    return result;
}

/* ---- the agent ----------------------------------------------------------------------------- */

int inkwell_ble_backend_agent_register(struct inkwell_ble_central *central) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                        "org.bluez.AgentManager1", "RegisterAgent");
    if (message == NULL) {
        return -ENOMEM;
    }
    const char *path = INKWELL_BLUEZ_AGENT_PATH;
    /* KeyboardDisplay is what makes a peripheral with a display choose passkey entry: its own
       capability is DisplayOnly, so BlueZ asks us for the number rather than the other way
       round. */
    const char *capability = "KeyboardDisplay";
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 2000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            /* An agent left behind by a previous run of the same process is the common case;
               treat it as registered rather than losing pairing for the session. */
            if (error.name != NULL && strcmp(error.name, "org.bluez.Error.AlreadyExists") == 0) {
                dbus_error_free(&error);
                return 0;
            }
            inkwell_log_warn("ble", "RegisterAgent failed: %s", error.message);
            mapped = dbus_error_to_errno(&error);
            dbus_error_free(&error);
        }
        return mapped;
    }
    dbus_message_unref(reply);

    /* Being the default agent is what routes requests here on a system with no other agent
       running. It is not fatal if something else already claimed it. */
    DBusMessage *request = dbus_message_new_method_call(
        "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
    if (request != NULL) {
        dbus_message_append_args(request, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
        dbus_error_init(&error);
        DBusMessage *default_reply =
            dbus_connection_send_with_reply_and_block(connection, request, 2000, &error);
        dbus_message_unref(request);
        if (default_reply != NULL) {
            dbus_message_unref(default_reply);
        } else if (dbus_error_is_set(&error)) {
            inkwell_log_warn("ble", "RequestDefaultAgent failed: %s", error.message);
            dbus_error_free(&error);
        }
    }
    inkwell_log_info("ble", "Pairing agent registered at %s", path);
    return 0;
}

void inkwell_ble_backend_agent_unregister(struct inkwell_ble_central *central) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return;
    }
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent");
    if (message == NULL) {
        return;
    }
    const char *path = INKWELL_BLUEZ_AGENT_PATH;
    dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    (void)dbus_connection_send(connection, message, NULL);
    dbus_message_unref(message);
    dbus_connection_flush(connection);
}

static void send_and_release(struct inkwell_ble_central *central, DBusMessage *reply) {
    DBusConnection *connection = connection_of(central);
    if (reply == NULL) {
        return;
    }
    if (connection != NULL) {
        (void)dbus_connection_send(connection, reply, NULL);
        dbus_connection_flush(connection);
    }
    dbus_message_unref(reply);
}

int inkwell_ble_backend_agent_answer(struct inkwell_ble_central *central, bool accept,
                                     uint32_t passkey) {
    struct bluez_backend *backend = backend_of(central);
    DBusMessage *call = backend->agent_pending;
    if (call == NULL) {
        return -ENOENT;
    }
    DBusMessage *reply = NULL;
    if (!accept) {
        reply = dbus_message_new_error(call, "org.bluez.Error.Rejected", "Cancelled");
    } else {
        reply = dbus_message_new_method_return(call);
        if (reply == NULL) {
            return -ENOMEM;
        }
        const enum inkwell_ble_agent_request_kind kind = central->agent_request.kind;
        if (kind == INKWELL_BLE_AGENT_REQUEST_PASSKEY) {
            dbus_uint32_t value = (dbus_uint32_t)passkey;
            dbus_message_append_args(reply, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID);
        } else if (kind == INKWELL_BLE_AGENT_REQUEST_PINCODE) {
            char digits[16];
            snprintf(digits, sizeof digits, "%06u", (unsigned)passkey);
            const char *text = digits;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
        }
        /* CONFIRM takes an empty reply: sending it *is* the confirmation. */
        inkwell_log_info("ble", "Answered pairing request for %s", central->agent_request.address);
    }
    send_and_release(central, reply);
    dbus_message_unref(call);
    backend->agent_pending = NULL;
    return 0;
}

static void agent_ack(struct inkwell_ble_central *central, DBusMessage *call) {
    send_and_release(central, dbus_message_new_method_return(call));
}

/*
 * Whether an agent call is about the bond this central has in flight.
 *
 * Being the *default* agent means BlueZ routes every question here - for pairings someone else
 * started (a remote device pairing to this host, another client's Pair) and for services on other
 * devices entirely. Answering one of those, or putting its PIN in front of the user as if it were
 * ours, would approve a bond nobody here asked for. So only the pair in flight is answered.
 */
static bool agent_call_is_ours(struct inkwell_ble_central *central, const char *device) {
    char ours[INKWELL_BLE_HANDLE_MAX];
    return central->pair_state == 1 && device != NULL &&
           device_path(central, central->pair_address, ours, sizeof ours) &&
           strcmp(device, ours) == 0;
}

static void agent_refuse(struct inkwell_ble_central *central, DBusMessage *call, const char *member,
                         const char *device) {
    inkwell_log_warn("ble", "Refusing %s for %s: no pairing of ours is in flight", member,
                     device != NULL ? device : "?");
    send_and_release(central,
                     dbus_message_new_error(call, "org.bluez.Error.Rejected", "Not requested"));
}

/* Holds an agent call that needs an answer from the user. BlueZ blocks the pairing until then
   (its own timeout is a minute, which is plenty to read a PIN off a screen and type it). */
static void agent_defer(struct inkwell_ble_central *central, DBusMessage *call,
                        enum inkwell_ble_agent_request_kind kind, const char *path,
                        uint32_t passkey) {
    /* One at a time: a stale request can only be one BlueZ has already given up on. */
    if (central->agent_request.kind != INKWELL_BLE_AGENT_REQUEST_NONE) {
        (void)inkwell_ble_agent_reject(central);
    }
    backend_of(central)->agent_pending = dbus_message_ref(call);
    central->agent_request.kind = kind;
    central->agent_request.passkey = passkey;
    address_from_path(path, central->agent_request.address, sizeof central->agent_request.address);
}

static const char k_agent_introspection[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node><interface name=\"org.bluez.Agent1\">"
    "<method name=\"Release\"/>"
    "<method name=\"RequestPinCode\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"out\"/></method>"
    "<method name=\"DisplayPinCode\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"RequestPasskey\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"DisplayPasskey\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"in\"/><arg type=\"q\" direction=\"in\"/></method>"
    "<method name=\"RequestConfirmation\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"in\"/></method>"
    "<method name=\"RequestAuthorization\"><arg type=\"o\" direction=\"in\"/></method>"
    "<method name=\"AuthorizeService\"><arg type=\"o\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"Cancel\"/></interface></node>";

/* org.bluez.Agent1, dispatched by hand. True when the message was for the agent. */
static bool handle_agent_call(struct inkwell_ble_central *central, DBusMessage *message) {
    const char *object = dbus_message_get_path(message);
    if (object == NULL || strcmp(object, INKWELL_BLUEZ_AGENT_PATH) != 0) {
        return false;
    }
    const char *member = dbus_message_get_member(message);
    if (member == NULL) {
        return true;
    }

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        DBusMessage *reply = dbus_message_new_method_return(message);
        if (reply != NULL) {
            const char *xml = k_agent_introspection;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        }
        send_and_release(central, reply);
        return true;
    }
    if (!dbus_message_has_interface(message, "org.bluez.Agent1")) {
        return true;
    }

    const char *device = NULL;
    dbus_uint32_t passkey = 0U;

    if (strcmp(member, "RequestPasskey") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        if (!agent_call_is_ours(central, device)) {
            agent_refuse(central, message, member, device);
            return true;
        }
        inkwell_log_info("ble", "Pairing: %s is asking for its PIN", device != NULL ? device : "?");
        agent_defer(central, message, INKWELL_BLE_AGENT_REQUEST_PASSKEY, device, 0U);
        return true;
    }
    if (strcmp(member, "RequestPinCode") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        if (!agent_call_is_ours(central, device)) {
            agent_refuse(central, message, member, device);
            return true;
        }
        agent_defer(central, message, INKWELL_BLE_AGENT_REQUEST_PINCODE, device, 0U);
        return true;
    }
    if (strcmp(member, "RequestConfirmation") == 0) {
        dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32,
                              &passkey, DBUS_TYPE_INVALID);
        if (!agent_call_is_ours(central, device)) {
            agent_refuse(central, message, member, device);
            return true;
        }
        /* Numeric comparison. The number is shown to the user rather than accepted blind: it
           is the only thing that says the bond is with the peripheral in your hand and not
           something else that answered the pairing. */
        agent_defer(central, message, INKWELL_BLE_AGENT_REQUEST_CONFIRM, device, (uint32_t)passkey);
        return true;
    }
    if (strcmp(member, "DisplayPasskey") == 0 || strcmp(member, "DisplayPinCode") == 0) {
        /* The peripheral is the one entering; nothing for us to do but say so in the log. */
        inkwell_log_info("ble", "Pairing: %s wants a code entered on it", member);
        agent_ack(central, message);
        return true;
    }
    if (strcmp(member, "RequestAuthorization") == 0 || strcmp(member, "AuthorizeService") == 0) {
        /* The two carry different argument lists, and get_args fails on a mismatch - which
           would leave `device` NULL and refuse the legitimate case along with the rest. */
        if (strcmp(member, "AuthorizeService") == 0) {
            const char *uuid = NULL;
            dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING,
                                  &uuid, DBUS_TYPE_INVALID);
        } else {
            dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        }
        if (!agent_call_is_ours(central, device)) {
            agent_refuse(central, message, member, device);
            return true;
        }
        agent_ack(central, message);
        return true;
    }
    if (strcmp(member, "Cancel") == 0) {
        /* BlueZ gave up on the request; the held call must not be answered any more. */
        struct bluez_backend *backend = backend_of(central);
        if (backend->agent_pending != NULL) {
            dbus_message_unref(backend->agent_pending);
            backend->agent_pending = NULL;
        }
        memset(&central->agent_request, 0, sizeof central->agent_request);
        inkwell_log_warn("ble", "Pairing request cancelled by BlueZ");
        agent_ack(central, message);
        return true;
    }
    if (strcmp(member, "Release") == 0) {
        central->agent_registered = false;
    }
    agent_ack(central, message);
    return true;
}

/* ---- GATT ---------------------------------------------------------------------------------- */

int inkwell_ble_backend_query(struct inkwell_ble_central *central, const char *address,
                              size_t which, uint32_t *token) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    char path[INKWELL_BLE_HANDLE_MAX];
    if (!device_path(central, address, path, sizeof path)) {
        return -EINVAL;
    }
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (message == NULL) {
        return -ENOMEM;
    }
    const char *interface = "org.bluez.Device1";
    const char *property = which == 1U ? "ServicesResolved" : "Connected";
    if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING,
                                  &property, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return -ENOMEM;
    }
    /* Queued without flushing: the writable watch drains the connection. */
    dbus_uint32_t serial = 0U;
    const dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -ENOMEM;
    }
    *token = serial;
    return 0;
}

int inkwell_ble_backend_write(struct inkwell_ble_central *central, const char *handle,
                              const uint8_t *data, size_t len, uint32_t *token) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", handle, "org.bluez.GattCharacteristic1", "WriteValue");
    if (message == NULL) {
        return -ENOMEM;
    }
    DBusMessageIter iter;
    DBusMessageIter bytes;
    DBusMessageIter options;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &bytes);
    if (len > 0U &&
        !dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data, (int)len)) {
        dbus_message_iter_abandon_container(&iter, &bytes);
        dbus_message_unref(message);
        return -ENOMEM;
    }
    dbus_message_iter_close_container(&iter, &bytes);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options);
    dbus_message_iter_close_container(&iter, &options);

    dbus_uint32_t serial = 0U;
    const dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -ENOMEM;
    }
    *token = serial;
    return 0;
}

int inkwell_ble_backend_read(struct inkwell_ble_central *central, const char *handle,
                             uint32_t *token) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", handle, "org.bluez.GattCharacteristic1", "ReadValue");
    if (message == NULL) {
        return -ENOMEM;
    }
    DBusMessageIter iter;
    DBusMessageIter options;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options);
    dbus_message_iter_close_container(&iter, &options);

    dbus_uint32_t serial = 0U;
    const dbus_bool_t sent = dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
    if (!sent) {
        return -ENOMEM;
    }
    /* The writable watch drains the queue. A flush here would block the loop on a congested
       bus. */
    *token = serial;
    return 0;
}

int inkwell_ble_backend_subscribe(struct inkwell_ble_central *central, const char *handle) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message = dbus_message_new_method_call(
        "org.bluez", handle, "org.bluez.GattCharacteristic1", "StartNotify");
    if (message == NULL) {
        return -ENOMEM;
    }
    /*
     * Deliberately not the 25 s default. StartNotify on an encrypted characteristic can make
     * BlueZ start a pairing, and BlueZ then calls our agent - which cannot be answered from
     * inside a blocking call, because the loop that pops messages is this thread. A caller that
     * bonds up front (pair_begin) never meets this; the timeout bounds the stall for the case
     * it still can, a bond that has gone stale on the peripheral.
     */
    const int result = call_blocking(central, message, 8000, "StartNotify");
    if (result < 0) {
        return result;
    }
    char rule[256];
    snprintf(rule, sizeof rule,
             "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
             "member='PropertiesChanged',path='%s'",
             handle);
    DBusError error;
    dbus_error_init(&error);
    dbus_bus_add_match(connection, rule, &error);
    if (dbus_error_is_set(&error)) {
        inkwell_log_warn("ble", "Failed to add notification match for %s: %s", handle,
                         error.message);
        dbus_error_free(&error);
    }
    dbus_connection_flush(connection);
    return 0;
}

int inkwell_ble_backend_mtu(struct inkwell_ble_central *central, const char *handle,
                            uint16_t *out_mtu) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    DBusMessage *message =
        dbus_message_new_method_call("org.bluez", handle, "org.freedesktop.DBus.Properties", "Get");
    if (message == NULL) {
        return -ENOMEM;
    }
    const char *interface = "org.bluez.GattCharacteristic1";
    const char *property = "MTU";
    if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING,
                                  &property, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return -ENOMEM;
    }
    DBusError error;
    dbus_error_init(&error);
    /* Once per connection, and only by a caller about to spend minutes on this link, so a
       second-long blocking call is the lookup's bargain again. */
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(connection, message, 1000, &error);
    dbus_message_unref(message);
    if (reply == NULL) {
        int mapped = -EIO;
        if (dbus_error_is_set(&error)) {
            /* A BlueZ without the property answers InvalidArgs, which is "not supported". */
            mapped = (error.name != NULL &&
                      strcmp(error.name, "org.freedesktop.DBus.Error.InvalidArgs") == 0)
                         ? -ENOTSUP
                         : dbus_error_to_errno(&error);
            inkwell_log_warn("ble", "MTU unavailable on %s: %s", handle, error.message);
            dbus_error_free(&error);
        }
        return mapped;
    }
    int result = -EPROTO;
    DBusMessageIter iter;
    DBusMessageIter value;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &value);
        if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT16) {
            dbus_uint16_t mtu = 0U;
            dbus_message_iter_get_basic(&value, &mtu);
            *out_mtu = (uint16_t)mtu;
            result = mtu > 0U ? 0 : -ENOTSUP;
        }
    }
    dbus_message_unref(reply);
    return result;
}

/* ---- replies ------------------------------------------------------------------------------- */

static int read_reply(DBusMessage *reply, uint8_t *out, size_t capacity, size_t *out_len) {
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        /* A read is the first thing to fail when a link is dying, so this branch is the whole
           account of most disconnects - and it used to answer -EIO without ever looking at what
           BlueZ said, which made "the link timed out", "the peripheral dropped its bond" and
           "bluetoothd is wedged" the same line in a log. Say what BlueZ said and map it. */
        return reply_error(reply, "ReadValue");
    }
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_BYTE) {
        return -EPROTO;
    }
    DBusMessageIter array;
    dbus_message_iter_recurse(&iter, &array);
    const uint8_t *payload = NULL;
    int length = 0;
    dbus_message_iter_get_fixed_array(&array, &payload, &length);
    if (length < 0 || (size_t)length > capacity) {
        return -EMSGSIZE;
    }
    if (length > 0) {
        memcpy(out, payload, (size_t)length);
    }
    *out_len = (size_t)length;
    return 0;
}

static void handle_properties_changed(struct inkwell_ble_central *central, DBusMessage *message) {
    const char *object = dbus_message_get_path(message);
    DBusMessageIter iter;
    if (object == NULL || !dbus_message_iter_init(message, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
        return;
    }
    const char *interface_name = NULL;
    dbus_message_iter_get_basic(&iter, &interface_name);
    if (interface_name != NULL && strcmp(interface_name, "org.bluez.Device1") == 0) {
        objects_properties_changed(backend_of(central), object, &iter);
        return;
    }
    if (central->notify_handle[0] == '\0' || strcmp(object, central->notify_handle) != 0) {
        return;
    }
    if (interface_name == NULL || strcmp(interface_name, "org.bluez.GattCharacteristic1") != 0 ||
        !dbus_message_iter_next(&iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter changed;
    dbus_message_iter_recurse(&iter, &changed);
    while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&changed, &entry);
        const char *property_name = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &property_name);
        }
        if (property_name == NULL || strcmp(property_name, "Value") != 0 ||
            !dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&changed);
            continue;
        }
        DBusMessageIter variant;
        dbus_message_iter_recurse(&entry, &variant);
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&variant) != DBUS_TYPE_BYTE) {
            dbus_message_iter_next(&changed);
            continue;
        }
        /* get_fixed_array wants an iterator positioned inside the array, not on it. */
        DBusMessageIter bytes;
        dbus_message_iter_recurse(&variant, &bytes);
        const uint8_t *payload = NULL;
        int length = 0;
        dbus_message_iter_get_fixed_array(&bytes, &payload, &length);
        if (payload != NULL && length > 0) {
            inkwell_ble_notify(central, payload, (size_t)length);
        }
        break;
    }
}

/* The reply to a write (index 0) or a property query (1, 2). */
static int request_reply(DBusMessage *message, size_t index, bool *value) {
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        return -EIO;
    }
    if (index == 0U) {
        return strcmp(dbus_message_get_signature(message), "") == 0 ? 0 : -EPROTO;
    }
    DBusMessageIter iter;
    DBusMessageIter variant;
    if (dbus_message_iter_init(message, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t boolean = FALSE;
            dbus_message_iter_get_basic(&variant, &boolean);
            *value = boolean != FALSE;
            return 0;
        }
    }
    return -EPROTO;
}

static void handle_message(struct inkwell_ble_central *central, DBusMessage *message) {
    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
        handle_agent_call(central, message)) {
        return;
    }

    struct bluez_backend *backend = backend_of(central);
    const dbus_uint32_t serial = dbus_message_get_reply_serial(message);
    if (serial != 0U && serial == backend->objects_serial) {
        backend->objects_serial = 0U;
        (void)objects_load(backend, message);
        return;
    }
    if (serial != 0U) {
        for (size_t i = 0; i < INKWELL_ARRAY_LEN(central->requests); ++i) {
            struct inkwell_ble_pending *request = &central->requests[i];
            if (request->state == 1 && request->token == serial) {
                const int result = request_reply(message, i, &request->value);
                inkwell_ble_pending_finish(request, result);
                return;
            }
        }
        if (central->read_state == 1 && central->read_token == serial) {
            const int result = read_reply(message, central->read_payload,
                                          sizeof central->read_payload, &central->read_length);
            inkwell_ble_read_finish(central, result);
            return;
        }
        if (central->pair_state == 1 && central->pair_token == serial) {
            inkwell_ble_pair_finish(central, dbus_message_get_type(message) ==
                                                     DBUS_MESSAGE_TYPE_METHOD_RETURN
                                                 ? 0
                                                 : reply_error(message, "Pair"));
            return;
        }
        if (central->connect_state == 1 && central->connect_token == serial) {
            inkwell_ble_connect_finish(central, dbus_message_get_type(message) ==
                                                        DBUS_MESSAGE_TYPE_METHOD_RETURN
                                                    ? 0
                                                    : reply_error(message, "Connect"));
            return;
        }
    }

    if (objects_handle_signal(central, message)) {
        return;
    }
    if (dbus_message_is_signal(message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        handle_properties_changed(central, message);
    }
}

int inkwell_ble_backend_process(struct inkwell_ble_central *central) {
    DBusConnection *connection = connection_of(central);
    if (connection == NULL) {
        return -ENOTCONN;
    }
    dbus_connection_read_write(connection, 0);
    DBusMessage *message = NULL;
    while ((message = dbus_connection_pop_message(connection)) != NULL) {
        handle_message(central, message);
        dbus_message_unref(message);
    }
    return 0;
}
