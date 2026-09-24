#pragma once

/*
 * A Bluetooth Low Energy central: find peripherals, connect to one, bond with it, and talk to
 * its GATT characteristics - all of it on the loop, none of it blocking for longer than a
 * bounded lookup.
 *
 * One interface over whichever stack the operating system has. On Linux that is BlueZ, over
 * D-Bus; on macOS it is CoreBluetooth; on Windows it is the Windows Runtime's LE API; anywhere
 * else, or on a Linux built without the D-Bus headers, every call refuses with -ENOSYS and the
 * caller reports Bluetooth as unavailable. The backend is chosen when inkwell is built, not at
 * run time: a process has exactly one Bluetooth stack under it.
 *
 * **A peripheral is named by its address** - the string the stack itself uses for it, and the
 * one a caller shows, stores and hands back. On BlueZ that is the controller address
 * ("FB:17:7C:37:6D:DA"), and on Windows it is the same hardware address. On macOS it is not, and
 * cannot be: CoreBluetooth never reveals a peripheral's hardware address and names it by a UUID
 * of its own instead, stable on that Mac and meaningless on any other. A caller should treat the
 * address as opaque, size its buffers by INKWELL_BLE_ADDRESS_MAX, and not assume it has colons in
 * it.
 *
 * **A characteristic is named by a handle** that inkwell_ble_find_characteristic() hands out.
 * It is an opaque string: on BlueZ it is the D-Bus object path, on CoreBluetooth and Windows
 * something that locates the characteristic under its peripheral. It stays good until that
 * peripheral disconnects.
 *
 * **Asynchrony is by polling, not by callback.** Connect, pair, read, write and the two
 * property queries each start a request and return; the reply is picked up by
 * inkwell_ble_process() on a later turn of the loop and handed back by the next call of the
 * same function (a `_poll`, or the call itself answering -EAGAIN until it is done). Two
 * wake-ups, `read_ready` and `requests_ready`, say when it is worth asking again. It is shaped
 * this way because every one of these can take seconds - a connect to a peripheral at the edge
 * of range, a pair waiting on a human typing a PIN - and nothing may hold the loop for that
 * long.
 *
 * A read, a write and the two property queries each have a deadline and answer -ETIMEDOUT past
 * it, whether or not the stack ever replies. **A connect and a pair do not**, and the caller owns
 * their clock: a pair waits on a human typing a PIN for as long as that takes, and how long a
 * connect is worth waiting for is a policy. A caller gives up with connect_cancel() or
 * pair_cancel(), which also frees the slot the next begin needs. Either way, a reply that arrives
 * after its request was timed out or cancelled is discarded rather than completing whatever was
 * asked next.
 *
 * Discovery on and off, a disconnect and set_trusted() are sent and not waited for: 0 means
 * the stack was asked, and a refusal that comes back later is logged, not returned.
 *
 * Some calls do block the loop, each for a bounded time: the first adapter, device or
 * characteristic lookup after the stack starts (one second - later ones read a copy the stack
 * keeps current), and on BlueZ the MTU lookup (one second) and forget (five).
 *
 * Only one of each kind of request is in flight at a time - one read, one write, one connect,
 * one pair - which is what a single link needs and what keeps the bookkeeping here a handful of
 * fields. A caller that wants two peripherals at once holds two centrals.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* A UUID string (36) and its terminator, which is the longest a backend's address gets. */
#define INKWELL_BLE_ADDRESS_MAX 40U
#define INKWELL_BLE_NAME_MAX 64U
#define INKWELL_BLE_HANDLE_MAX 128U
/* The largest attribute value ATT allows (Core spec, Vol 3, Part F, 3.2.9). */
#define INKWELL_BLE_VALUE_MAX 512U

struct inkwell_ble_device {
    char address[INKWELL_BLE_ADDRESS_MAX];
    char name[INKWELL_BLE_NAME_MAX];
    int16_t rssi;
    /* The stack holds a bond with it. A peripheral that encrypts its characteristics answers
       a subscribe with "not paired" until it has one, so a caller needs to be able to say so
       before the user presses connect. Always true on CoreBluetooth, which bonds on demand
       with a dialog of its own and never says whether it has: there is no pair step to take. */
    bool paired;
    /*
     * Whether the stack heard the peripheral in the current scan, which is the only evidence
     * in a listing that it is actually within earshot.
     *
     * A listing includes every peripheral the stack *holds*, and a bond outlives the peer
     * being in the room - so the one on a desk at home is in the list all day,
     * indistinguishable by address, name or `paired` from the one in your pocket. BlueZ drops
     * the RSSI property from a device it has not heard in the current discovery session, so
     * its presence is the range test and its absence is a bond with nothing behind it.
     *
     * It is a separate field rather than `rssi != 0` because 0 is a legal reading and, worse,
     * a *high* one: every real measurement is negative, so an absent RSSI left in an int16
     * outranks every peripheral that answered.
     */
    bool in_range;
};

/*
 * Parameters for an LE connection interval update, in controller units. Intervals use 1.25 ms;
 * the supervision timeout uses 10 ms. The central validates the Bluetooth Core bounds before
 * asking the backend, so invalid commands never reach the controller.
 */
struct inkwell_ble_connection_parameters {
    uint16_t min_interval;
    uint16_t max_interval;
    uint16_t latency;
    uint16_t supervision_timeout;
};

/*
 * What the stack is asking the user while a pair is in flight.
 *
 * On BlueZ the central registers an agent with KeyboardDisplay capability, so a peripheral
 * whose own capability is DisplayOnly - one that shows a PIN on its screen - picks passkey
 * entry, and BlueZ blocks the pairing until the digits arrive. The reply is deferred until the
 * user has typed them, which is the whole reason pairing can happen inside an application.
 *
 * CoreBluetooth never asks: macOS raises its own pairing dialog, and no request of this kind is
 * ever pending there.
 */
enum inkwell_ble_agent_request_kind {
    INKWELL_BLE_AGENT_REQUEST_NONE = 0,
    INKWELL_BLE_AGENT_REQUEST_PASSKEY, /* six digits, entered by us */
    INKWELL_BLE_AGENT_REQUEST_PINCODE, /* legacy BR/EDR string PIN */
    INKWELL_BLE_AGENT_REQUEST_CONFIRM, /* numeric comparison: `passkey` is shown on both ends */
};

struct inkwell_ble_agent_request {
    enum inkwell_ble_agent_request_kind kind;
    char address[INKWELL_BLE_ADDRESS_MAX];
    uint32_t passkey; /* CONFIRM only: the number the peripheral is displaying */
};

typedef void (*inkwell_ble_notification_callback)(const uint8_t *data, size_t len, void *userdata);

struct inkwell_ble_central;

/* One request in flight: a write, or one of the two property queries. `token` is how the
   backend recognises its reply; 0 means none is expected, so a late one is dropped. */
struct inkwell_ble_pending {
    struct inkwell_ble_central *central;
    uint32_t token;
    int state; /* 0 idle, 1 pending, 2 done */
    int result;
    int timer_fd;
    uint64_t deadline_ms;
    bool value;
};

/*
 * The fields a caller sets are the three at the top. Everything below them is the central's
 * own; it is in the header so a caller can hold one by value, and a test can reach a timer to
 * expire it early, not to be read.
 */
struct inkwell_ble_central {
    /* Called when a read has completed and when a write or property query has - which is the
       moment to call it again for the answer. Optional; process() also enforces deadlines, so a
       caller that polls on its own tick needs neither. */
    void (*read_ready)(void *userdata);
    void (*requests_ready)(void *userdata);
    void *userdata;

    /* [0] write, [1] services-resolved, [2] connected, [3] subscribe. */
    struct inkwell_ble_pending requests[4];
    uint32_t read_token;
    int read_state; /* 0 idle, 1 pending, 2 done */
    int read_result;
    int read_timer_fd;
    uint64_t read_deadline_ms;
    unsigned read_mock_polls;
    uint8_t read_payload[INKWELL_BLE_VALUE_MAX];
    size_t read_length;

    bool open;
    struct inkwell_loop *loop;
    inkwell_ble_notification_callback notification_callback;
    void *notification_userdata;
    char notify_handle[INKWELL_BLE_HANDLE_MAX];
    /* The characteristic a subscribe in flight is for; notify_handle once it is confirmed. */
    char subscribe_handle[INKWELL_BLE_HANDLE_MAX];

    uint32_t connect_token;
    int connect_state; /* 0 idle, 1 pending, 2 done (see connect_result) */
    int connect_result;
    /* Pairing takes as long as the user needs to read a PIN off the peripheral and type it
       in, so it can never be a blocking call. */
    uint32_t pair_token;
    int pair_state;
    int pair_result;
    char pair_address[INKWELL_BLE_ADDRESS_MAX];

    bool agent_registered;
    struct inkwell_ble_agent_request agent_request;

    /* The adapter find_adapter() chose, in the backend's own spelling. */
    char adapter[INKWELL_BLE_HANDLE_MAX];
    /* The backend's state; NULL until open. */
    void *backend;
};

/* Opens the central on the system's Bluetooth stack. -ENOSYS when inkwell was built without
   one. The central is zeroed first, so the three caller fields are set *after* this. */
int inkwell_ble_open(struct inkwell_ble_central *central);
/*
 * The same on a connection of its own.
 *
 * On BlueZ every caller in a process is otherwise handed one shared system-bus connection, and
 * a central installs its watch functions on it and pops every message off it - so a second
 * central on the shared one takes the first one's replies and notifications away from it. Use
 * this for any central that can be open at the same time as another. Elsewhere it is the same
 * as inkwell_ble_open().
 */
int inkwell_ble_open_private(struct inkwell_ble_central *central);
void inkwell_ble_close(struct inkwell_ble_central *central);

/* Whether the stack is there to be asked: 0, -ENODEV when it is not running (bluetoothd has no
   owner on the bus), -EAGAIN while it is still starting (CoreBluetooth before the radio has
   reported its state), -EACCES when this process may not use it. */
int inkwell_ble_check_ready(struct inkwell_ble_central *central);
/* Picks the adapter to use and names it in `name`, for a log. -ENODEV when there is none. */
int inkwell_ble_find_adapter(struct inkwell_ble_central *central, char *name, size_t name_len);
/* Sent, not waited for. -ENETDOWN at once when the stack already knows the adapter is off. */
int inkwell_ble_start_discovery(struct inkwell_ble_central *central);
int inkwell_ble_stop_discovery(struct inkwell_ble_central *central);
/*
 * Whether the adapter is scanning, as the stack reports it: 1 or 0, -EAGAIN while it has not
 * said, -ENOSYS where there is no stack.
 *
 * The two calls above are sent and not waited for, so 0 from them means only that the stack
 * was asked. A refusal arrives later and is logged - BlueZ answers InProgress to a start that
 * lands on a stop still settling - and a caller keeping its own "scanning" flag from the request
 * then believes in a scan that is not running, hears nothing, and waits on it forever. Compare
 * this against what was asked for and ask again when they disagree.
 */
int inkwell_ble_discovering(struct inkwell_ble_central *central);
/* Every peripheral the stack holds that advertises `service_uuid`, compared without regard to
   case. Read from memory, except that on BlueZ the first lookup of any kind after bluetoothd
   starts fetches its object tree, bounded to a second. */
int inkwell_ble_list_by_service(struct inkwell_ble_central *central, const char *service_uuid,
                                struct inkwell_ble_device *devices, size_t capacity, size_t *count);

/* Starts a connect and returns at once. -EBUSY if one is already in flight. No deadline: see the
   top of this file. */
int inkwell_ble_connect_begin(struct inkwell_ble_central *central, const char *address);
/* 1 when the reply has arrived (*out_result 0 or a negative errno), 0 while pending, -EINVAL if
   nothing is in flight. */
int inkwell_ble_connect_poll(struct inkwell_ble_central *central, int *out_result);
/* Forgets an in-flight connect (a late reply is then ignored). Safe when none is pending. */
void inkwell_ble_connect_cancel(struct inkwell_ble_central *central);
/* Sent, not waited for. */
int inkwell_ble_disconnect(struct inkwell_ble_central *central, const char *address);

/* Starts a pair and returns at once. -EBUSY if one is already in flight. Where the stack pairs
   on its own (CoreBluetooth bonds when an encrypted characteristic is first used) there is
   nothing to start, and the pair simply finishes on the next turn. */
int inkwell_ble_pair_begin(struct inkwell_ble_central *central, const char *address);
/* As connect_poll(). */
int inkwell_ble_pair_poll(struct inkwell_ble_central *central, int *out_result);
/* Abandons an in-flight pair: rejects whatever the agent is holding and asks the stack to
   cancel. */
void inkwell_ble_pair_cancel(struct inkwell_ble_central *central);
/* Marks the peripheral trusted, so the stack reconnects to it without asking again. Sent, not
   waited for; a no-op where the stack has no such notion. */
int inkwell_ble_set_trusted(struct inkwell_ble_central *central, const char *address, bool trusted);
/* Drops the bond and everything the stack remembers about the peripheral. -ENOTSUP where an
   application may not (on macOS only System Settings can forget a device). */
int inkwell_ble_forget(struct inkwell_ble_central *central, const char *address);

/* Registers the pairing agent (idempotent). Safe to call when the stack has no agent manager;
   failures are reported, not fatal. */
int inkwell_ble_agent_register(struct inkwell_ble_central *central);
void inkwell_ble_agent_unregister(struct inkwell_ble_central *central);
/* What the agent is blocked on, if anything: true and *out filled when a request is pending. */
bool inkwell_ble_agent_request(const struct inkwell_ble_central *central,
                               struct inkwell_ble_agent_request *out);
/* Answers a pending PASSKEY (or PINCODE, formatted as six digits). -ENOENT when nothing is
   waiting. */
int inkwell_ble_agent_submit_passkey(struct inkwell_ble_central *central, uint32_t passkey);
/* Accepts a pending CONFIRM. -ENOENT when nothing is waiting. */
int inkwell_ble_agent_confirm(struct inkwell_ble_central *central);
/* Rejects whatever is pending, which refuses the bond. */
int inkwell_ble_agent_reject(struct inkwell_ble_central *central);

/* Whether the link to `address` is up. False once the stack has seen the peripheral drop it.
   -EAGAIN while the query is in flight: call again on a later turn. */
int inkwell_ble_device_connected(struct inkwell_ble_central *central, const char *address,
                                 bool *out_connected);
/* Whether GATT service discovery has finished. A connect completes once the link is up, but
   the characteristics are only there to be found after discovery, which can take several
   seconds when the stack has nothing cached. Poll this before find_characteristic().
   -EAGAIN while the query is in flight. */
int inkwell_ble_services_resolved(struct inkwell_ble_central *central, const char *address,
                                  bool *out_resolved);
/* The handle of one characteristic under `address`, by UUID. -ENOENT when the peripheral has
   resolved no such characteristic. Blocks only as list_by_service() does. */
int inkwell_ble_find_characteristic(struct inkwell_ble_central *central, const char *address,
                                    const char *char_uuid, char *out_handle, size_t out_len);
/*
 * The ATT MTU negotiated for the link this characteristic is on.
 *
 * A write carries `mtu - 3` bytes as one Write Request; one byte more and BlueZ quietly turns
 * it into a Prepare/Execute long write, which is several writes to a peripheral that may count
 * them. So anything that paces a peripheral per write needs this number rather than a guess.
 * -ENOTSUP when the stack does not publish it (BlueZ did not before 5.62).
 */
int inkwell_ble_characteristic_mtu(struct inkwell_ble_central *central, const char *handle,
                                   uint16_t *out_mtu);
/*
 * Whether the kernel still holds an LE link to `address`, in any state: 1 or 0.
 *
 * Not the same question as Device1.Connected. A disconnect the controller never confirms leaves
 * the kernel's connection in BT_DISCONN after bluetoothd has already said the device is gone, and
 * every Connect to it then fails at once with "Operation already in progress" - seen on Linux 4.9
 * when one peripheral is disconnected and another connected within a second or two. A caller
 * moving from one peripheral to another can ask this before connecting, and tell that failure
 * from a peripheral that is merely busy. -ENOTSUP where the stack does not expose it; on Linux it
 * opens a raw HCI socket, which needs CAP_NET_RAW.
 */
int inkwell_ble_link_held(struct inkwell_ble_central *central, const char *address);
/*
 * Resets the adapter's controller (HCIDEVRESET, what `hciconfig hci0 reset` does), which drops
 * every link and every connection the kernel was holding - the one way out of the state above.
 * Everything on the adapter goes down with it, discovery included, so it is a last resort.
 * -ENOTSUP where the stack does not expose it; on Linux it needs CAP_NET_RAW for the raw HCI
 * socket it is sent on as well as CAP_NET_ADMIN for the reset itself.
 */
int inkwell_ble_reset_adapter(struct inkwell_ble_central *central);
/*
 * Asks the controller to change the interval on the open LE link to `address`.
 *
 * Call this after selecting an adapter and opening the connection. BlueZ has no D-Bus method for
 * this, so its backend resolves the connection handle and sends an HCI LE Connection Update
 * directly. The command is handed to the controller, but its later completion event is not
 * awaited. -ENOENT means there is no open LE link to the address; -ENOTSUP means this stack does
 * not expose interval control. On Linux the raw HCI socket also requires CAP_NET_RAW.
 */
int inkwell_ble_request_connection_interval(
    struct inkwell_ble_central *central, const char *address,
    const struct inkwell_ble_connection_parameters *parameters);
/*
 * Turns on notifications from one characteristic; each arrives at the notification handler.
 * One subscription at a time.
 *
 * As write(): starts it and answers -EAGAIN, and later calls answer -EAGAIN until the stack has
 * confirmed, then 0 or a negative errno - -EACCES when the peripheral wants a bond first. It
 * waits on a peripheral that may start a pairing to answer, and that may put a question in front
 * of the user, so the deadline is the stack's: eight seconds on BlueZ, thirty on CoreBluetooth,
 * where the answer can be behind macOS's pairing dialog.
 */
int inkwell_ble_subscribe(struct inkwell_ble_central *central, const char *handle);
/* A write with response. Starts it and answers -EAGAIN; later calls answer -EAGAIN until the
   reply is in and then return it, 0 or a negative errno. A caller keeps what it is writing at
   the head of its queue until that reply has been consumed. */
int inkwell_ble_write(struct inkwell_ble_central *central, const char *handle, const uint8_t *data,
                      size_t len);
/* Starts a read or consumes its completion, as write() does. -EAGAIN means pending, never a
   failed read. An empty value is a successful read of zero bytes. */
int inkwell_ble_read(struct inkwell_ble_central *central, const char *handle, uint8_t *out,
                     size_t capacity, size_t *out_len);
void inkwell_ble_read_cancel(struct inkwell_ble_central *central);
/* Cancels the write and both property queries, for a link being discarded. */
void inkwell_ble_requests_cancel(struct inkwell_ble_central *central);

/* Puts the central's descriptors on `loop`. Without it the central still works, but only as
   often as the caller calls process(). */
int inkwell_ble_attach_loop(struct inkwell_ble_central *central, struct inkwell_loop *loop);
void inkwell_ble_detach_loop(struct inkwell_ble_central *central);
/* Picks up whatever the stack has answered and enforces deadlines. The loop calls it when a
   descriptor fires; a caller may call it on its own tick as well. */
int inkwell_ble_process(struct inkwell_ble_central *central);
void inkwell_ble_set_notification_handler(struct inkwell_ble_central *central,
                                          inkwell_ble_notification_callback callback,
                                          void *userdata);

/*
 * A scripted stack for tests, standing in for the real one in every call above.
 *
 * Tests must never reach a real Bluetooth stack. Enabling the mock makes every central opened
 * afterwards talk to this script instead, and it is process-wide on purpose: the code under
 * test opens its own centrals and a test cannot hand it one.
 */
struct inkwell_ble_mock_config {
    /*
     * Routes the backend-specific half - reads, writes, property queries, the adapter, device
     * and characteristic lookups, discovery, disconnect and trust - to a real backend on this
     * bus instead of the script, with everything else still scripted. BlueZ
     * only: it exists so the D-Bus marshalling and the reply bookkeeping can be tested against
     * a fake org.bluez on an isolated bus under dbus-run-session. Never the system bus.
     */
    const char *bus_address;
    int open_result;
    int check_ready_result;
    int find_adapter_result;
    const char *adapter_name;
    int request_connection_interval_result;
    unsigned *request_connection_interval_calls;
    /* inkwell_ble_link_held(): the first `link_held_queries` answers are 1, then 0 - a kernel
       that takes that many polls to let go of a link. */
    unsigned link_held_queries;
    unsigned *link_held_calls;
    int reset_adapter_result;
    unsigned *reset_adapter_calls;
    int start_discovery_result;
    int stop_discovery_result;
    /* A start that answers 0 and does not scan: BlueZ's InProgress refusal, which arrives after
       the call has returned. inkwell_ble_discovering() is the only thing that tells. */
    bool start_discovery_lost;
    /* Bumped on every start/stop, so a test can assert that a scan is down for the whole of a
       link rather than only that it was stopped once. */
    unsigned *start_discovery_calls;
    unsigned *stop_discovery_calls;
    int connect_result;
    /*
     * Models a device that lives only as long as the scan that found it: once discovery has
     * stopped, a connect answers -ENOENT the way bluetoothd answers UnknownObject for a path it
     * has just dropped. Off by default, because a bonded peripheral outlives its scan - it is a
     * peripheral this adapter has never seen before that does not.
     */
    bool connect_needs_the_scan;
    int disconnect_result;
    int pair_result;
    int forget_result;
    unsigned *forget_calls;
    /* Pair polls that stay pending before the mock completes with pair_result. */
    unsigned pair_pending_polls;
    /* When set, a pair raises a PASSKEY request the way BlueZ would, so a test can drive the
       whole PIN flow. */
    bool pair_requests_passkey;
    /* The passkey the caller answered with. */
    uint32_t *pair_passkey_capture;
    /* Bumped every time an agent registration actually reaches the mock, so a test can tell a
       fresh registration from one short-circuited by a stale flag. */
    unsigned *agent_register_calls;
    int write_result;
    int subscribe_result;
    /* Subscribe calls that answer -EAGAIN before one answers subscribe_result. */
    unsigned subscribe_pending_polls;
    /* services_resolved() polls that report false before the mock flips to true (0 = resolved
       on the first poll, i.e. the stack had the GATT database cached). */
    unsigned services_resolved_after_polls;
    int services_resolved_result;
    /* Polls that answer -ETIMEDOUT before that sequence starts, standing in for a stack too
       busy to answer a property query inside its deadline. */
    unsigned services_resolved_timeout_polls;
    /* Connect polls that stay pending before the mock completes with connect_result. */
    unsigned connect_pending_polls;
    /* device_connected() polls that report true before the mock reports the link dropped
       (0 = never drops). */
    unsigned connected_drops_after_polls;
    /* Writes that succeed before the mock starts failing them with write_result_late
       (0 = write_result applies to every call). */
    unsigned write_fail_after_calls;
    int write_result_late;
    /* Scripted reads: each returns the next payload, then empty. */
    const uint8_t *const *read_payloads;
    const size_t *read_payload_lengths;
    size_t read_payload_count;
    size_t *read_index;
    int read_result;
    unsigned read_pending_polls; /* simulated latency; zero completes immediately */
    const struct inkwell_ble_device *devices;
    size_t device_count;
    int list_result;
    /* Counts listings, so a test can pin how often a caller makes one. */
    unsigned *list_calls;
    uint8_t *write_capture_buffer;
    size_t write_capture_capacity;
    size_t *write_capture_length;
    char *write_capture_handle;
    size_t write_capture_handle_capacity;
    size_t *write_call_count;
    size_t *write_lengths;
    size_t write_lengths_capacity;
    /* The service each of `devices` advertises, parallel to it. When set, a listing returns
       only the devices whose entry matches; NULL keeps every device answering every listing. */
    const char *const *device_service_uuids;
    /* The MTU every characteristic reports. 0 is a stack that does not publish one. */
    uint16_t mtu;
    /* Called with every write that succeeds, after the captures above. A fake peripheral
       answers from here by queueing notifications for the test to emit, never by emitting them
       inline: a real answer arrives on a later turn of the loop, and one delivered inside the
       write would be a peripheral faster than light. */
    void (*write_hook)(void *userdata, const char *handle, const uint8_t *data, size_t len);
    void *write_hook_userdata;
};

/*
 * A mock characteristic's handle is "<address>/<uuid>", as the caller spelled both - so a test
 * can name the handle a write or a notification belongs to without asking for it.
 */
void inkwell_ble_mock_enable(const struct inkwell_ble_mock_config *config);
void inkwell_ble_mock_disable(void);
/* Delivers a notification to the central the mock last connected, if `handle` is the one it
   subscribed to (or is NULL). */
void inkwell_ble_mock_emit_notification(const char *handle, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
