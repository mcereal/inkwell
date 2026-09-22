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
         "agent ownership passed.");
    return 0;
}
