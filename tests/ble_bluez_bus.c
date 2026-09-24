#define _POSIX_C_SOURCE 200809L

/*
 * The BlueZ backend's D-Bus half, against a fake org.bluez: that reads, writes and property
 * queries are sent without blocking, marshalled the way bluetoothd expects, answered from their
 * replies, timed out when there is none, and never completed by a reply that arrives late.
 *
 * Run only under dbus-run-session: the fake service and the central use its isolated address,
 * never the host's system bus or real BlueZ.
 */
#include "inkwell/ble/central.h"
#include "inkwell/runtime/loop.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void ready(void *userdata) {
    ++*(unsigned *)userdata;
}
static int input(int fd, uint32_t events, void *userdata) {
    (void)events;
    uint64_t count;
    if (read(fd, &count, sizeof count) != sizeof count)
        return -EIO;
    ++*(unsigned *)userdata;
    return 0;
}

static DBusMessage *request_named(DBusConnection *server, struct inkwell_loop *loop,
                                  const char *member) {
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        inkwell_loop_run(loop, 0);
        dbus_connection_read_write(server, 10);
        DBusMessage *message;
        while ((message = dbus_connection_pop_message(server)) != NULL) {
            if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
                strcmp(dbus_message_get_member(message), member) == 0) {
                return message;
            }
            dbus_message_unref(message);
        }
    }
    return NULL;
}

static void respond(DBusConnection *server, DBusMessage *call, bool malformed) {
    DBusMessage *reply = dbus_message_new_method_return(call);
    const uint8_t payload[] = {0x08, 0x01, 0x12, 0x00};
    const uint8_t *ptr = payload;
    const char *wrong = "wrong type";
    if (malformed) {
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong, DBUS_TYPE_INVALID);
    } else {
        dbus_message_append_args(reply, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &ptr, (int)sizeof payload,
                                 DBUS_TYPE_INVALID);
    }
    dbus_connection_send(server, reply, NULL);
    dbus_connection_flush(server);
    dbus_message_unref(reply);
}

#define ADDRESS "AA:BB:CC:DD:EE:FF"

static int operation(struct inkwell_ble_central *client, unsigned op, bool *value) {
    const uint8_t data[] = {0x08, 0x01};
    if (op == 0U)
        return inkwell_ble_write(client, "/characteristic", data, sizeof data);
    if (op == 1U)
        return inkwell_ble_services_resolved(client, ADDRESS, value);
    return inkwell_ble_device_connected(client, ADDRESS, value);
}

static void operation_reply(DBusConnection *server, DBusMessage *call, unsigned op, unsigned pass) {
    DBusMessage *reply = pass == 1U
                             ? dbus_message_new_error(call, "org.bluez.Error.Failed", "failed")
                             : dbus_message_new_method_return(call);
    if (pass == 2U) {
        const char *wrong = "wrong";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong, DBUS_TYPE_INVALID);
    } else if (op != 0U && pass != 1U) {
        DBusMessageIter iter, variant;
        dbus_bool_t value = TRUE;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
        dbus_message_iter_close_container(&iter, &variant);
    }
    dbus_connection_send(server, reply, NULL);
    dbus_connection_flush(server);
    dbus_message_unref(reply);
}

static const char *test_operations(DBusConnection *server, struct inkwell_loop *loop,
                                   struct inkwell_ble_central *client, int input_fd,
                                   unsigned *inputs) {
    for (unsigned op = 0U; op < 3U; ++op) {
        for (unsigned pass = 0U; pass < 5U; ++pass) {
            bool value = false;
            if (operation(client, op, &value) != -EAGAIN)
                return "operation did not yield";
            DBusMessage *call = request_named(server, loop, op == 0U ? "WriteValue" : "Get");
            if (call == NULL)
                return "operation never reached fake service";
            const bool signature =
                strcmp(dbus_message_get_signature(call), op == 0U ? "aya{sv}" : "ss") == 0;
            const unsigned before = *inputs;
            const uint64_t one = 1U;
            if (write(input_fd, &one, sizeof one) != sizeof one) {
                dbus_message_unref(call);
                return "input wake failed";
            }
            inkwell_loop_run(loop, 0);
            if (!signature || *inputs != before + 1U || operation(client, op, &value) != -EAGAIN) {
                dbus_message_unref(call);
                return "pending operation blocked input, duplicated send or marshalled incorrectly";
            }
            if (pass >= 3U) {
                if (pass == 3U) {
                    const struct itimerspec spec = {.it_value = {.tv_nsec = 1L}};
                    timerfd_settime(client->requests[op].timer_fd, 0, &spec, NULL);
                    struct pollfd fd = {.fd = client->requests[op].timer_fd, .events = POLLIN};
                    (void)poll(&fd, 1, 1000);
                    inkwell_loop_run(loop, 0);
                    if (operation(client, op, &value) != -ETIMEDOUT) {
                        dbus_message_unref(call);
                        return "operation timeout lost";
                    }
                } else {
                    inkwell_ble_requests_cancel(client);
                }
                /* A reply for the old link/request cannot finish the new request. */
                if (operation(client, op, &value) != -EAGAIN) {
                    dbus_message_unref(call);
                    return "operation did not restart";
                }
                DBusMessage *next = request_named(server, loop, op == 0U ? "WriteValue" : "Get");
                operation_reply(server, call, op, 0U);
                dbus_message_unref(call);
                call = next;
                inkwell_loop_run(loop, 1);
                if (call == NULL || operation(client, op, &value) != -EAGAIN) {
                    if (call != NULL)
                        dbus_message_unref(call);
                    return "late reply completed a newer operation";
                }
            }
            operation_reply(server, call, op, pass < 3U ? pass : 0U);
            dbus_message_unref(call);
            for (unsigned turn = 0U; turn < 100U && client->requests[op].state == 1; ++turn) {
                inkwell_loop_run(loop, 1);
            }
            const int expected = pass == 1U ? -EIO : pass == 2U ? -EPROTO : 0;
            if (operation(client, op, &value) != expected ||
                (expected == 0 && op != 0U && !value)) {
                return "operation reply result incorrect";
            }
        }
    }
    return NULL;
}

/* ---- the object tree ---------------------------------------------------------------------- */

#define SERVICE "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
#define CHARACTERISTIC "2c55e69e-4993-11ed-b878-0242ac120002"
#define DEVICE_PATH "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
#define CHARACTERISTIC_PATH DEVICE_PATH "/service0010/char0011"

static void append_property(DBusMessageIter *dict, const char *name, int type, const void *value) {
    char signature[2] = {(char)type, '\0'};
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/* One interface's a{sv}. A device carries an address, a name, a bond, an RSSI and one service;
   a characteristic its UUID; an adapter nothing. */
static void append_interface(DBusMessageIter *interfaces, const char *interface,
                             const char *address) {
    DBusMessageIter entry, properties;
    dbus_message_iter_open_container(interfaces, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &properties);
    if (strcmp(interface, "org.bluez.Device1") == 0) {
        const char *name = "Node";
        const dbus_bool_t paired = TRUE;
        const int16_t rssi = -60;
        append_property(&properties, "Address", DBUS_TYPE_STRING, &address);
        append_property(&properties, "Name", DBUS_TYPE_STRING, &name);
        append_property(&properties, "Paired", DBUS_TYPE_BOOLEAN, &paired);
        append_property(&properties, "RSSI", DBUS_TYPE_INT16, &rssi);
        const char *key = "UUIDs";
        const char *uuid = SERVICE;
        DBusMessageIter property, variant, uuids;
        dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, NULL, &property);
        dbus_message_iter_append_basic(&property, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&property, DBUS_TYPE_VARIANT, "as", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &uuids);
        dbus_message_iter_append_basic(&uuids, DBUS_TYPE_STRING, &uuid);
        dbus_message_iter_close_container(&variant, &uuids);
        dbus_message_iter_close_container(&property, &variant);
        dbus_message_iter_close_container(&properties, &property);
    } else if (strcmp(interface, "org.bluez.GattCharacteristic1") == 0) {
        const char *uuid = CHARACTERISTIC;
        append_property(&properties, "UUID", DBUS_TYPE_STRING, &uuid);
    }
    dbus_message_iter_close_container(&entry, &properties);
    dbus_message_iter_close_container(interfaces, &entry);
}

/* One object and its interface, as GetManagedObjects lists it. */
static void append_object(DBusMessageIter *objects, const char *path, const char *interface,
                          const char *address) {
    DBusMessageIter entry, interfaces;
    dbus_message_iter_open_container(objects, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sa{sv}}", &interfaces);
    append_interface(&interfaces, interface, address);
    dbus_message_iter_close_container(&entry, &interfaces);
    dbus_message_iter_close_container(objects, &entry);
}

static void emit(DBusConnection *server, DBusMessage *signal) {
    dbus_connection_send(server, signal, NULL);
    dbus_connection_flush(server);
    dbus_message_unref(signal);
}

static void emit_interfaces(DBusConnection *server, bool added, const char *path,
                            const char *interface, const char *address) {
    DBusMessage *signal = dbus_message_new_signal("/", "org.freedesktop.DBus.ObjectManager",
                                                  added ? "InterfacesAdded" : "InterfacesRemoved");
    DBusMessageIter iter, array;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    if (added) {
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &array);
        append_interface(&array, interface, address);
    } else {
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &array);
        dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &interface);
    }
    dbus_message_iter_close_container(&iter, &array);
    emit(server, signal);
}

/* One of the adapter's boolean properties changing, as bluetoothd reports it. */
static void emit_adapter_flag(DBusConnection *server, const char *name, bool on) {
    DBusMessage *signal = dbus_message_new_signal(
        "/org/bluez/hci0", "org.freedesktop.DBus.Properties", "PropertiesChanged");
    const char *interface = "org.bluez.Adapter1";
    const dbus_bool_t value = on ? TRUE : FALSE;
    DBusMessageIter iter, changed, invalidated;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed);
    append_property(&changed, name, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&iter, &changed);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&iter, &invalidated);
    emit(server, signal);
}

/* The adapter switched on or off. */
static void emit_powered(DBusConnection *server, bool powered) {
    emit_adapter_flag(server, "Powered", powered);
}

/* A rename, and the RSSI going: what bluetoothd says when a device stops being heard. */
static void emit_renamed_and_unheard(DBusConnection *server) {
    DBusMessage *signal = dbus_message_new_signal(DEVICE_PATH, "org.freedesktop.DBus.Properties",
                                                  "PropertiesChanged");
    const char *interface = "org.bluez.Device1";
    const char *name = "Renamed";
    const char *rssi = "RSSI";
    DBusMessageIter iter, changed, invalidated;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed);
    append_property(&changed, "Name", DBUS_TYPE_STRING, &name);
    dbus_message_iter_close_container(&iter, &changed);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_append_basic(&invalidated, DBUS_TYPE_STRING, &rssi);
    dbus_message_iter_close_container(&iter, &invalidated);
    emit(server, signal);
}

/* Everything the server sent before this has been handled by the client once it returns: a
   read's reply comes after them on the same connection. */
static void settle(DBusConnection *server, struct inkwell_loop *loop,
                   struct inkwell_ble_central *client) {
    uint8_t bytes[16];
    size_t length;
    if (inkwell_ble_read(client, "/characteristic", bytes, sizeof bytes, &length) != -EAGAIN)
        return;
    DBusMessage *call = request_named(server, loop, "ReadValue");
    if (call != NULL) {
        respond(server, call, false);
        dbus_message_unref(call);
    }
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        inkwell_loop_run(loop, 1);
        if (inkwell_ble_read(client, "/characteristic", bytes, sizeof bytes, &length) != -EAGAIN)
            return;
    }
}

static bool request_name(DBusConnection *server) {
    return dbus_bus_request_name(server, "org.bluez", 0, NULL) ==
           DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
}

/*
 * A listing reads a copy of the object tree, fetched once and kept current by signals - never a
 * GetManagedObjects per call. A bluetoothd that restarts is fetched again, without blocking, and
 * one that is gone empties the copy rather than leaving its devices listed.
 */
static const char *test_object_tree(DBusConnection *server, struct inkwell_loop *loop,
                                    struct inkwell_ble_central *client) {
    /* A restart: the name goes and comes back, and the client asks for the new tree. */
    dbus_bus_release_name(server, "org.bluez", NULL);
    if (!request_name(server))
        return "could not take org.bluez back";
    DBusMessage *call = request_named(server, loop, "GetManagedObjects");
    if (call == NULL)
        return "a restarted bluetoothd was not asked for its tree";
    DBusMessage *reply = dbus_message_new_method_return(call);
    dbus_message_unref(call);
    DBusMessageIter iter, objects;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects);
    append_object(&objects, "/org/bluez/hci0", "org.bluez.Adapter1", NULL);
    append_object(&objects, DEVICE_PATH, "org.bluez.Device1", ADDRESS);
    append_object(&objects, CHARACTERISTIC_PATH, "org.bluez.GattCharacteristic1", NULL);
    dbus_message_iter_close_container(&iter, &objects);
    emit(server, reply);
    settle(server, loop, client);

    /* Each of these blocks for a second and fails without the copy: the fake answers nothing
       while the client is waiting on it. */
    char adapter[64];
    char handle[INKWELL_BLE_HANDLE_MAX];
    struct inkwell_ble_device devices[4];
    size_t count = 0U;
    if (inkwell_ble_find_adapter(client, adapter, sizeof adapter) != 0 ||
        strcmp(adapter, "/org/bluez/hci0") != 0)
        return "the adapter was not read from the copy";
    if (inkwell_ble_list_by_service(client, SERVICE, devices, 4U, &count) != 0 || count != 1U ||
        strcmp(devices[0].address, ADDRESS) != 0 || strcmp(devices[0].name, "Node") != 0 ||
        !devices[0].paired || devices[0].rssi != -60 || !devices[0].in_range)
        return "the listing was not read from the copy";
    if (inkwell_ble_find_characteristic(client, ADDRESS, CHARACTERISTIC, handle, sizeof handle) !=
            0 ||
        strcmp(handle, CHARACTERISTIC_PATH) != 0)
        return "the characteristic was not read from the copy";

    emit_renamed_and_unheard(server);
    emit_interfaces(server, true, "/org/bluez/hci0/dev_11_22_33_44_55_66", "org.bluez.Device1",
                    "11:22:33:44:55:66");
    emit_interfaces(server, false, CHARACTERISTIC_PATH, "org.bluez.GattCharacteristic1", NULL);
    settle(server, loop, client);
    if (inkwell_ble_list_by_service(client, SERVICE, devices, 4U, &count) != 0 || count != 2U ||
        strcmp(devices[0].name, "Renamed") != 0 || devices[0].in_range ||
        strcmp(devices[1].address, "11:22:33:44:55:66") != 0)
        return "a property change or an added device did not reach the copy";
    if (inkwell_ble_find_characteristic(client, ADDRESS, CHARACTERISTIC, handle, sizeof handle) !=
        -ENOENT)
        return "a removed characteristic was still found";

    emit_interfaces(server, false, "/org/bluez/hci0/dev_11_22_33_44_55_66", "org.bluez.Device1",
                    NULL);
    settle(server, loop, client);
    if (inkwell_ble_list_by_service(client, SERVICE, devices, 4U, &count) != 0 || count != 1U)
        return "a removed device was still listed";

    /* Gone: nothing is listed from the old tree, and asking finds nobody to answer. */
    dbus_bus_release_name(server, "org.bluez", NULL);
    settle(server, loop, client);
    if (inkwell_ble_list_by_service(client, SERVICE, devices, 4U, &count) != -EIO)
        return "a vanished bluetoothd's devices were still listed";
    if (!request_name(server))
        return "could not take org.bluez back";
    return NULL;
}

/*
 * Discovery, Disconnect and Trusted are sent and not waited for: each returns 0 while the fake
 * has not answered - a blocking call would have timed out - and reaches it afterwards. A refusal
 * that comes back later is logged and changes nothing - which is why whether the adapter is
 * scanning is read from what bluetoothd reports, not from what was asked. An adapter the copy
 * knows is off refuses discovery at once.
 */
static const char *test_sent_not_waited(DBusConnection *server, struct inkwell_loop *loop,
                                        struct inkwell_ble_central *client) {
    /* test_object_tree() left bluetoothd restarted, with its tree still to be sent. */
    DBusMessage *call = request_named(server, loop, "GetManagedObjects");
    if (call == NULL)
        return "no GetManagedObjects to answer";
    DBusMessage *reply = dbus_message_new_method_return(call);
    dbus_message_unref(call);
    DBusMessageIter iter, objects;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects);
    append_object(&objects, "/org/bluez/hci0", "org.bluez.Adapter1", NULL);
    dbus_message_iter_close_container(&iter, &objects);
    emit(server, reply);
    settle(server, loop, client);

    static const char *const members[] = {"StartDiscovery", "StopDiscovery", "Disconnect", "Set"};
    for (unsigned i = 0U; i < 4U; ++i) {
        const int result = i == 0U   ? inkwell_ble_start_discovery(client)
                           : i == 1U ? inkwell_ble_stop_discovery(client)
                           : i == 2U ? inkwell_ble_disconnect(client, ADDRESS)
                                     : inkwell_ble_set_trusted(client, ADDRESS, true);
        if (result != 0)
            return "a call that is only logged waited for its answer";
        call = request_named(server, loop, members[i]);
        if (call == NULL)
            return "a call that is only logged never reached the fake";
        DBusMessage *refusal = dbus_message_new_error(call, "org.bluez.Error.Failed", "refused");
        dbus_message_unref(call);
        emit(server, refusal);
    }
    settle(server, loop, client);
    if (inkwell_ble_discovering(client) != 0)
        return "a refused StartDiscovery was taken for a scan";

    emit_adapter_flag(server, "Discovering", true);
    settle(server, loop, client);
    if (inkwell_ble_discovering(client) != 1)
        return "the adapter reporting Discovering did not read as scanning";
    emit_adapter_flag(server, "Discovering", false);
    settle(server, loop, client);
    if (inkwell_ble_discovering(client) != 0)
        return "the adapter stopping did not read as not scanning";

    emit_powered(server, false);
    settle(server, loop, client);
    if (inkwell_ble_start_discovery(client) != -ENETDOWN)
        return "discovery on an adapter that is off was not refused";
    emit_powered(server, true);
    settle(server, loop, client);
    if (inkwell_ble_start_discovery(client) != 0)
        return "discovery was still refused once the adapter was back on";
    return NULL;
}

static void notified(const uint8_t *data, size_t len, void *userdata) {
    (void)data;
    if (len > 0U)
        ++*(unsigned *)userdata;
}

/* A value on `path`, as bluetoothd sends a notification. */
static void emit_value(DBusConnection *server, const char *path) {
    DBusMessage *signal =
        dbus_message_new_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    const char *interface = "org.bluez.GattCharacteristic1";
    const char *key = "Value";
    const uint8_t bytes[] = {0x2a};
    const uint8_t *ptr = bytes;
    DBusMessageIter iter, changed, entry, variant, array, invalidated;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed);
    dbus_message_iter_open_container(&changed, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &array);
    dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &ptr, (int)sizeof bytes);
    dbus_message_iter_close_container(&variant, &array);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&changed, &entry);
    dbus_message_iter_close_container(&iter, &changed);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&iter, &invalidated);
    emit(server, signal);
}

/*
 * StartNotify is sent and not waited for - the agent it can set off is answered by the same loop
 * - and answers -EAGAIN until the reply: a refusal as the errno BlueZ's words map to, a success
 * as 0, after which the characteristic's values reach the handler.
 */
static const char *test_subscribe(DBusConnection *server, struct inkwell_loop *loop,
                                  struct inkwell_ble_central *client) {
    unsigned values = 0U;
    inkwell_ble_set_notification_handler(client, notified, &values);
    const char *failure = NULL;
    /* Pass 1 is the kernel's EALREADY as Device1.Connect and StartNotify both pass it on. */
    static const char *const refusals[] = {"Not paired", "Operation already in progress"};
    static const int errnos[] = {-EACCES, -EALREADY, 0};
    for (unsigned pass = 0U; pass < 3U && failure == NULL; ++pass) {
        if (inkwell_ble_subscribe(client, CHARACTERISTIC_PATH) != -EAGAIN) {
            failure = "subscribe did not yield";
            break;
        }
        DBusMessage *call = request_named(server, loop, "StartNotify");
        if (call == NULL || inkwell_ble_subscribe(client, CHARACTERISTIC_PATH) != -EAGAIN) {
            failure = "StartNotify never reached the fake, or was answered without it";
            if (call != NULL)
                dbus_message_unref(call);
            break;
        }
        DBusMessage *reply =
            pass < 2U ? dbus_message_new_error(call, "org.bluez.Error.Failed", refusals[pass])
                      : dbus_message_new_method_return(call);
        dbus_message_unref(call);
        emit(server, reply);
        int result = -EAGAIN;
        for (unsigned turn = 0U; turn < 100U && result == -EAGAIN; ++turn) {
            inkwell_loop_run(loop, 1);
            result = inkwell_ble_subscribe(client, CHARACTERISTIC_PATH);
        }
        if (result != errnos[pass])
            failure = "the StartNotify reply was not returned as the right errno";
    }
    if (failure == NULL) {
        emit_value(server, CHARACTERISTIC_PATH);
        settle(server, loop, client);
        if (values != 1U)
            failure = "a value on the subscribed characteristic did not reach the handler";
    }
    inkwell_ble_set_notification_handler(client, NULL, NULL);
    return failure;
}

/*
 * The default agent is asked about every pairing on the host, not only ours. A RequestPasskey for
 * a device this central is not pairing must be refused on the bus and never become a prompt.
 */
static const char *test_agent_refuses_strangers(DBusConnection *server, struct inkwell_loop *loop,
                                                struct inkwell_ble_central *client,
                                                const char *client_name) {
    DBusMessage *ask = dbus_message_new_method_call(client_name, "/org/inkwell/agent",
                                                    "org.bluez.Agent1", "RequestPasskey");
    const char *device = "/org/bluez/hci0/dev_11_22_33_44_55_66";
    dbus_message_append_args(ask, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
    dbus_uint32_t serial = 0U;
    dbus_connection_send(server, ask, &serial);
    dbus_connection_flush(server);
    dbus_message_unref(ask);

    const char *failure = "the agent never answered a stranger's RequestPasskey";
    for (unsigned turn = 0U; turn < 100U; ++turn) {
        inkwell_loop_run(loop, 0);
        (void)inkwell_ble_process(client);
        dbus_connection_read_write(server, 10);
        DBusMessage *message;
        while ((message = dbus_connection_pop_message(server)) != NULL) {
            if (dbus_message_get_reply_serial(message) == serial) {
                const char *error = dbus_message_get_error_name(message);
                failure = error != NULL && strcmp(error, "org.bluez.Error.Rejected") == 0
                              ? NULL
                              : "a stranger's RequestPasskey was not rejected";
                turn = 100U;
            }
            dbus_message_unref(message);
        }
    }
    if (failure == NULL && inkwell_ble_agent_request(client, NULL)) {
        failure = "a stranger's RequestPasskey became a prompt";
    }
    return failure;
}

int main(void) {
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address == NULL) {
        fputs("This test requires dbus-run-session.\n", stderr);
        return 1;
    }
    DBusError error;
    dbus_error_init(&error);
    DBusConnection *server = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (server == NULL || dbus_bus_request_name(server, "org.bluez", 0, &error) !=
                              DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fputs("Could not start isolated fake service.\n", stderr);
        return 1;
    }
    struct inkwell_ble_mock_config mock = {.bus_address = address};
    inkwell_ble_mock_enable(&mock);
    struct inkwell_ble_central client;
    struct inkwell_loop loop;
    if (inkwell_loop_init(&loop) != 0 || inkwell_ble_open(&client) != 0 ||
        inkwell_ble_attach_loop(&client, &loop) != 0) {
        fputs("Could not initialize isolated client.\n", stderr);
        return 1;
    }
    unsigned completions = 0U;
    unsigned inputs = 0U;
    client.read_ready = ready;
    client.userdata = &completions;
    int input_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    inkwell_loop_add_fd(&loop, input_fd, INKWELL_LOOP_IN, input, &inputs);
    const char *failure = NULL;
    uint8_t bytes[512];
    size_t length;
    DBusMessage *call = NULL;
    /* The central's name on the bus, read off the first call it makes; the agent test calls
       back on it. */
    char client_name[64] = {0};
    for (unsigned pass = 0U; pass < 4U; ++pass) {
        if (inkwell_ble_read(&client, "/characteristic", bytes, sizeof bytes, &length) != -EAGAIN ||
            length != 0U) {
            failure = "read did not yield";
            break;
        }
        call = request_named(server, &loop, "ReadValue");
        if (call == NULL) {
            failure = "queued read never reached the fake service";
            break;
        }
        if (client_name[0] == '\0' && dbus_message_get_sender(call) != NULL) {
            snprintf(client_name, sizeof client_name, "%s", dbus_message_get_sender(call));
        }
        if (strcmp(dbus_message_get_signature(call), "a{sv}") != 0) {
            failure = "ReadValue options were not marshalled correctly";
            break;
        }
        const uint64_t one = 1U;
        if (write(input_fd, &one, sizeof one) != sizeof one) {
            failure = "input wake failed";
            break;
        }
        inkwell_loop_run(&loop, 0);
        if (inputs != pass + 1U || completions != pass) {
            failure = "input must be serviced while the reply is withheld";
            break;
        }
        if (pass == 2U) {
            const struct itimerspec spec = {.it_value = {.tv_nsec = 1L}};
            timerfd_settime(client.read_timer_fd, 0, &spec, NULL);
            struct pollfd fd = {.fd = client.read_timer_fd, .events = POLLIN};
            (void)poll(&fd, 1, 1000);
        } else {
            respond(server, call, pass == 1U);
        }
        for (unsigned turn = 0U; turn < 100U && completions == pass; ++turn) {
            inkwell_loop_run(&loop, 1);
        }
        const int result =
            inkwell_ble_read(&client, "/characteristic", bytes, sizeof bytes, &length);
        const int expected = pass == 1U ? -EPROTO : pass == 2U ? -ETIMEDOUT : 0;
        if (completions != pass + 1U || result != expected ||
            (result == 0 && (length != 4U || bytes[0] != 0x08U))) {
            failure = "reply, malformed payload or timeout completion was incorrect";
            break;
        }
        if (pass == 2U) {
            /* A timed-out reply must not complete the following read. */
            respond(server, call, false);
            inkwell_loop_run(&loop, 1);
            if (completions != pass + 1U) {
                failure = "late reply was not discarded";
                break;
            }
        }
        dbus_message_unref(call);
        call = NULL;
    }
    if (call != NULL) {
        dbus_message_unref(call);
    }
    if (failure == NULL) {
        failure = test_operations(server, &loop, &client, input_fd, &inputs);
    }
    if (failure == NULL) {
        failure = test_object_tree(server, &loop, &client);
    }
    if (failure == NULL) {
        failure = test_sent_not_waited(server, &loop, &client);
    }
    if (failure == NULL) {
        failure = test_subscribe(server, &loop, &client);
    }
    if (failure == NULL) {
        failure = client_name[0] != '\0'
                      ? test_agent_refuses_strangers(server, &loop, &client, client_name)
                      : "never learned the central's bus name";
    }
    inkwell_loop_remove_fd(&loop, input_fd);
    close(input_fd);
    inkwell_ble_close(&client);
    inkwell_loop_shutdown(&loop);
    inkwell_ble_mock_disable();
    dbus_connection_close(server);
    dbus_connection_unref(server);
    dbus_error_free(&error);
    if (failure != NULL) {
        fprintf(stderr, "%s\n", failure);
        return 1;
    }
    puts("Isolated D-Bus: nonblocking send, input responsiveness, reply parsing, timeout and "
         "agent ownership, object tree, unwaited calls, subscribe passed.");
    return 0;
}
