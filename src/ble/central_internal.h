#pragma once

/*
 * The seam between central.c and a backend. Not public: it declares only what would still be
 * `static` if the central were one file.
 *
 * central.c owns everything that is the same on every stack - argument checks, the mock, the
 * request bookkeeping, the timers and deadlines - and calls a backend only to *send* something
 * or to *look something up*. A backend never decides whether a request may start or when one
 * has expired; it records the token its stack will reply under, and when the reply comes in it
 * matches it against the central's tokens and calls one of the completion helpers below.
 *
 * Exactly one backend is linked: bluez.c where D-Bus was found on Linux, corebluetooth.m on
 * macOS, and none.c everywhere else. Each defines every function in the second half of this file.
 */

#include "inkwell/ble/central.h"

/* ---- central.c, for a backend ------------------------------------------------------------ */

/* A write or property query's reply. Drops it when the request has already been finished. */
void inkwell_ble_pending_finish(struct inkwell_ble_pending *request, int result);
/* A read's reply. `result` 0 means the value is in read_payload/read_length already. */
void inkwell_ble_read_finish(struct inkwell_ble_central *central, int result);
void inkwell_ble_connect_finish(struct inkwell_ble_central *central, int result);
void inkwell_ble_pair_finish(struct inkwell_ble_central *central, int result);
/* The stack's answer to a subscribe. On 0, notifications from it are delivered from now on. */
void inkwell_ble_subscribe_finish(struct inkwell_ble_central *central, int result);
/* A value from the subscribed characteristic. */
void inkwell_ble_notify(struct inkwell_ble_central *central, const uint8_t *data, size_t len);

/* ---- the backend, for central.c ---------------------------------------------------------- */

/* How long a write may go unanswered. The stack's to say, because what a write can wait behind
   differs: BlueZ bonds up front, CoreBluetooth encrypts a link on first use and may hold a
   write while it does - or while the user answers its pairing dialog. */
extern const unsigned inkwell_ble_backend_write_timeout_ms;
/* How long a subscribe may go unanswered, for the same reason. */
extern const unsigned inkwell_ble_backend_subscribe_timeout_ms;

/* `private_connection` and `bus_address` as inkwell_ble_open_private() and the mock's
   bus_address describe; a backend with no such notion ignores both. Allocates
   central->backend. */
int inkwell_ble_backend_open(struct inkwell_ble_central *central, bool private_connection,
                             const char *bus_address);
void inkwell_ble_backend_close(struct inkwell_ble_central *central);
int inkwell_ble_backend_check_ready(struct inkwell_ble_central *central);
/* Fills central->adapter and `name`. */
int inkwell_ble_backend_find_adapter(struct inkwell_ble_central *central, char *name,
                                     size_t name_len);
int inkwell_ble_backend_discovery(struct inkwell_ble_central *central, bool on);
/* 1 or 0 as the stack reports it, -EAGAIN while it has not said. */
int inkwell_ble_backend_discovering(struct inkwell_ble_central *central);
int inkwell_ble_backend_list_by_service(struct inkwell_ble_central *central,
                                        const char *service_uuid,
                                        struct inkwell_ble_device *devices, size_t capacity,
                                        size_t *count);

/* Each of these sends and records the token its reply will carry; central.c has already
   marked the request pending and armed its deadline, and cancels it if this fails. */
int inkwell_ble_backend_connect(struct inkwell_ble_central *central, const char *address,
                                uint32_t *token);
int inkwell_ble_backend_pair(struct inkwell_ble_central *central, const char *address,
                             uint32_t *token);
int inkwell_ble_backend_write(struct inkwell_ble_central *central, const char *handle,
                              const uint8_t *data, size_t len, uint32_t *token);
int inkwell_ble_backend_read(struct inkwell_ble_central *central, const char *handle,
                             uint32_t *token);
/* `which` is 1 for services-resolved, 2 for connected: the index into central->requests. */
int inkwell_ble_backend_query(struct inkwell_ble_central *central, const char *address,
                              size_t which, uint32_t *token);
/* central->subscribe_handle is already `handle`; the reply goes to subscribe_finish(). */
int inkwell_ble_backend_subscribe(struct inkwell_ble_central *central, const char *handle,
                                  uint32_t *token);

/* A connect or pair being abandoned while its reply is still out. */
void inkwell_ble_backend_connect_cancel(struct inkwell_ble_central *central);
void inkwell_ble_backend_pair_cancel(struct inkwell_ble_central *central);

int inkwell_ble_backend_disconnect(struct inkwell_ble_central *central, const char *address);
int inkwell_ble_backend_set_trusted(struct inkwell_ble_central *central, const char *address,
                                    bool trusted);
int inkwell_ble_backend_forget(struct inkwell_ble_central *central, const char *address);

int inkwell_ble_backend_agent_register(struct inkwell_ble_central *central);
void inkwell_ble_backend_agent_unregister(struct inkwell_ble_central *central);
/* Answers the request the agent is holding: accept with `passkey` (ignored for CONFIRM), or
   refuse. The backend releases whatever it held; central.c then clears agent_request. */
int inkwell_ble_backend_agent_answer(struct inkwell_ble_central *central, bool accept,
                                     uint32_t passkey);

int inkwell_ble_backend_find_characteristic(struct inkwell_ble_central *central,
                                            const char *address, const char *char_uuid,
                                            char *out_handle, size_t out_len);
int inkwell_ble_backend_mtu(struct inkwell_ble_central *central, const char *handle,
                            uint16_t *out_mtu);
int inkwell_ble_backend_request_connection_interval(
    struct inkwell_ble_central *central, const char *address,
    const struct inkwell_ble_connection_parameters *parameters);

int inkwell_ble_backend_attach(struct inkwell_ble_central *central);
void inkwell_ble_backend_detach(struct inkwell_ble_central *central);
/* Reads what the stack has sent and hands each reply to the helpers above. */
int inkwell_ble_backend_process(struct inkwell_ble_central *central);
