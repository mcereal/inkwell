#include "central_internal.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/wake.h"

/* winsock2.h before windows.h, as everywhere in inkwell; COBJMACROS for the C accessors, and
   INITGUID so the IIDs this file names are defined here rather than looked for in a library. */
#define COBJMACROS
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <robuffer.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
// clang-format on

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The Windows backend, over the Windows Runtime's Bluetooth LE API, from C.
 *
 * **Every WinRT call is made on the loop thread; the thread pool only knocks.** WinRT answers an
 * async operation and raises an event on a thread of its own, and there is no descriptor to wait
 * on instead - the same position CoreBluetooth's queue puts corebluetooth.m in, and the same
 * answer: nothing crosses between threads but copies.
 *
 * - An async operation's completion handler does no work. It queues "operation N finished" and
 *   signals an inkwell wake; the loop thread then calls GetResults() itself. WinRT's Bluetooth
 *   objects are agile, so a result may be collected on any thread, and collecting it on ours is
 *   what keeps every table here single-threaded.
 * - Two handlers read on the pool thread, and only into copies: an advertisement, into the table
 *   of peripherals heard (the one structure both threads touch, under the hub's lock), and a
 *   notification, into a queued event.
 * - The hub - lock, queue, wake, heard table - is reference counted, and every handler holds a
 *   reference. A handler can outlive the central (an operation WinRT finishes after a close), and
 *   finds the hub closed rather than freed.
 *
 * **Addresses are the hardware address**, "F8:5B:1B:A5:99:C9": Windows reports it, unlike macOS.
 * A handle is "<address>/<characteristic UUID>", as on CoreBluetooth.
 *
 * **Bonding is not a step here.** Windows 11 has been seen to fail to bond an ESP32 peripheral in
 * every pairing mode - its own Settings dialog included: the peripheral reports the passkey
 * authenticated and Windows gives up a moment later. So a peripheral lists as `paired`, as on
 * CoreBluetooth, pair_begin() finishes at once, and a characteristic that insists on a bond fails
 * with -EACCES, which a caller already reports as "pair it first".
 *
 * **MinGW's WinRT headers stop short** of the interfaces added after Windows 8.1 that this needs
 * (IBluetoothLEDevice3's GetGattServicesAsync). Those few are declared below, from the system's
 * own metadata: the IIDs are the ones Windows.Devices.winmd carries.
 */

#define BT(x) __x_ABI_CWindows_CDevices_CBluetooth_##x
#define ADV(x) __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_##x
#define GATT(x) __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_##x
#define STREAMS(x) __x_ABI_CWindows_CStorage_CStreams_##x

/* A write waits on nothing but the peripheral: Windows does not stop to bond (see above). */
const unsigned inkwell_ble_backend_write_timeout_ms = 10000U;
/* A subscribe is a write to the peripheral's descriptor, and the first one on a link can wait
   behind Windows discovering the rest of the database. */
const unsigned inkwell_ble_backend_subscribe_timeout_ms = 15000U;

/* How long a peripheral stays "in range" after it was last heard, as on CoreBluetooth. */
#define WINRT_IN_RANGE_MS 15000U
#define WINRT_HEARD_MAX 64U
#define WINRT_ADVERTISED_UUIDS 8U
#define WINRT_SERVICES_MAX 16U
#define WINRT_CHARACTERISTICS_MAX 64U

/*
 * Shorter names for the generated ones this file uses. MinGW spells a WinRT generic out in full -
 * an async-completion handler's IID runs to 150 characters - which no line limit survives.
 */
// clang-format off
#define IID_winrt_adapter_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CBluetoothAdapter
#define IID_winrt_device_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice
#define IID_winrt_services_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult
#define IID_winrt_characteristics_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristicsResult
#define IID_winrt_read_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattReadResult
#define IID_winrt_write_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattWriteResult
#define IID_winrt_session_done IID___FIAsyncOperationCompletedHandler_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattSession
#define IID_winrt_received_handler IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementWatcher_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEAdvertisementReceivedEventArgs
#define IID_winrt_value_handler IID___FITypedEventHandler_2_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattValueChangedEventArgs
#define IID_winrt_watcher IID___x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher
#define IID_winrt_characteristic3 IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic3
#define IID_winrt_service3 IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3
#define IID_winrt_session_statics IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSessionStatics
#define kWatcherClass RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher
#define kSessionClass RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattSession
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristicsResult winrt_characteristics_op;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattWriteResult winrt_write_op;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattReadResult winrt_read_op;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattSession winrt_session_op;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice winrt_device_op;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothAdapter winrt_adapter_op;
#define winrt_services_size __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_get_Size
#define winrt_services_at __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_GetAt
#define winrt_services_release __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_Release
typedef __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService winrt_services_view;
#define winrt_characteristics_size __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_get_Size
#define winrt_characteristics_at __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_GetAt
#define winrt_characteristics_release __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_Release
typedef __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic winrt_characteristics_view;
// clang-format on

/* ---- what MinGW does not declare ---------------------------------------------------------- */

#define WINRT_INSPECTABLE(T)                                                                       \
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(T * self, REFIID iid, void **out);                  \
    ULONG(STDMETHODCALLTYPE *AddRef)(T * self);                                                    \
    ULONG(STDMETHODCALLTYPE *Release)(T * self);                                                   \
    HRESULT(STDMETHODCALLTYPE *GetIids)(T * self, ULONG * count, IID * *iids);                     \
    HRESULT(STDMETHODCALLTYPE *GetRuntimeClassName)(T * self, HSTRING * name);                     \
    HRESULT(STDMETHODCALLTYPE *GetTrustLevel)(T * self, TrustLevel * level);

/* Windows.Devices.Bluetooth.IBluetoothLEDevice3 {aee9e493-44ac-40dc-af33-b2c13c01ca46}. */
DEFINE_GUID(IID_winrt_IBluetoothLEDevice3, 0xaee9e493, 0x44ac, 0x40dc, 0xaf, 0x33, 0xb2, 0xc1, 0x3c,
            0x01, 0xca, 0x46);

typedef struct winrt_le_device3 winrt_le_device3;
struct winrt_le_device3_vtbl {
    WINRT_INSPECTABLE(winrt_le_device3)
    HRESULT(STDMETHODCALLTYPE *get_DeviceAccessInformation)(winrt_le_device3 *self, void **out);
    HRESULT(STDMETHODCALLTYPE *RequestAccessAsync)(winrt_le_device3 *self, void **op);
    HRESULT(STDMETHODCALLTYPE *GetGattServicesAsync)(winrt_le_device3 *self, void **op);
    HRESULT(STDMETHODCALLTYPE *GetGattServicesWithCacheModeAsync)
    (winrt_le_device3 *self, INT32 mode, void **op);
    HRESULT(STDMETHODCALLTYPE *GetGattServicesForUuidAsync)
    (winrt_le_device3 *self, GUID uuid, void **op);
    HRESULT(STDMETHODCALLTYPE *GetGattServicesForUuidWithCacheModeAsync)
    (winrt_le_device3 *self, GUID uuid, INT32 mode, void **op);
};
struct winrt_le_device3 {
    const struct winrt_le_device3_vtbl *lpVtbl;
};

/*
 * Every IAsyncOperation<T> this file starts has the same shape - IInspectable, then
 * put_Completed, get_Completed, GetResults - and a T that is an interface pointer. One view of it
 * is what lets one completion handler serve them all; the IID it answers to is what differs.
 */
typedef struct winrt_async winrt_async;
struct winrt_async_vtbl {
    WINRT_INSPECTABLE(winrt_async)
    HRESULT(STDMETHODCALLTYPE *put_Completed)(winrt_async *self, void *handler);
    HRESULT(STDMETHODCALLTYPE *get_Completed)(winrt_async *self, void **handler);
    HRESULT(STDMETHODCALLTYPE *GetResults)(winrt_async *self, void **result);
};
struct winrt_async {
    const struct winrt_async_vtbl *lpVtbl;
};

/* ---- the hub: what the thread pool and the loop share ------------------------------------ */

enum winrt_op {
    WINRT_OP_ADAPTER,
    WINRT_OP_DEVICE,
    WINRT_OP_SERVICES,
    WINRT_OP_CHARACTERISTICS,
    WINRT_OP_SESSION,
    WINRT_OP_WRITE,
    WINRT_OP_READ,
    WINRT_OP_SUBSCRIBE,
};

enum winrt_event_kind {
    WINRT_EVENT_DONE,   /* an async operation finished: `completion` says which */
    WINRT_EVENT_NOTIFY, /* a value from the subscribed characteristic */
    WINRT_EVENT_ANSWER, /* a reply this file makes itself, delivered on the next turn */
};

enum winrt_answer {
    WINRT_ANSWER_CONNECT,
    WINRT_ANSWER_PAIR,
    WINRT_ANSWER_QUERY,
};

struct winrt_completion;

struct winrt_event {
    struct winrt_event *next;
    enum winrt_event_kind kind;
    struct winrt_completion *completion;
    enum winrt_answer answer;
    uint32_t token;
    size_t which;
    int result;
    bool value;
    size_t len;
    uint8_t data[INKWELL_BLE_VALUE_MAX];
};

struct winrt_heard {
    uint64_t address;
    char name[INKWELL_BLE_NAME_MAX];
    int16_t rssi;
    uint64_t heard_ms;
    GUID services[WINRT_ADVERTISED_UUIDS];
    size_t service_count;
};

struct winrt_hub {
    LONG refs;
    SRWLOCK lock;
    bool closed;
    struct inkwell_wake wake;
    struct winrt_event *head;
    struct winrt_event *tail;
    struct winrt_heard heard[WINRT_HEARD_MAX];
    size_t heard_count;
};

static void hub_retain(struct winrt_hub *hub) {
    (void)InterlockedIncrement(&hub->refs);
}

static void hub_release(struct winrt_hub *hub) {
    if (InterlockedDecrement(&hub->refs) == 0) {
        free(hub);
    }
}

/* Appends and knocks, unless the central has closed - the check and the signal both under the
   lock, so close() cannot slip between them and leave a signal aimed at a closed wake. On the
   loop thread too: a reply made here is still delivered on the next turn. */
static void hub_post(struct winrt_hub *hub, struct winrt_event *event) {
    AcquireSRWLockExclusive(&hub->lock);
    if (hub->closed) {
        ReleaseSRWLockExclusive(&hub->lock);
        free(event);
        return;
    }
    event->next = NULL;
    if (hub->tail != NULL) {
        hub->tail->next = event;
    } else {
        hub->head = event;
    }
    hub->tail = event;
    (void)inkwell_wake_signal(&hub->wake);
    ReleaseSRWLockExclusive(&hub->lock);
}

/* ---- the completion handler ------------------------------------------------------------- */

struct winrt_completion {
    const struct winrt_completion_vtbl *lpVtbl;
    LONG refs;
    struct winrt_hub *hub;
    const IID *iid;
    enum winrt_op op_kind;
    uint32_t token;
    size_t index;
    winrt_async *op;
    AsyncStatus status;
};

struct winrt_completion_vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)
    (struct winrt_completion *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(struct winrt_completion *self);
    ULONG(STDMETHODCALLTYPE *Release)(struct winrt_completion *self);
    HRESULT(STDMETHODCALLTYPE *Invoke)(struct winrt_completion *self, void *op, AsyncStatus status);
};

static ULONG STDMETHODCALLTYPE completion_add_ref(struct winrt_completion *self) {
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE completion_release(struct winrt_completion *self) {
    const LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        if (self->op != NULL) {
            (void)self->op->lpVtbl->Release(self->op);
        }
        hub_release(self->hub);
        free(self);
    }
    return (ULONG)refs;
}

/* IAgileObject says it may be called from any thread without marshalling, which is the whole of
   what it does. */
static HRESULT STDMETHODCALLTYPE completion_query(struct winrt_completion *self, REFIID iid,
                                                  void **out) {
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAgileObject) ||
        IsEqualIID(iid, self->iid)) {
        *out = self;
        (void)completion_add_ref(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE completion_invoke(struct winrt_completion *self, void *op,
                                                   AsyncStatus status) {
    (void)op;
    struct winrt_event *event = calloc(1U, sizeof *event);
    if (event == NULL) {
        return S_OK;
    }
    self->status = status;
    event->kind = WINRT_EVENT_DONE;
    event->completion = self;
    (void)completion_add_ref(self);
    hub_post(self->hub, event);
    return S_OK;
}

static const struct winrt_completion_vtbl kCompletionVtbl = {
    completion_query,
    completion_add_ref,
    completion_release,
    completion_invoke,
};

/* ---- the event listener ----------------------------------------------------------------- */

enum winrt_listener_kind {
    WINRT_LISTEN_ADVERTISEMENT,
    WINRT_LISTEN_VALUE,
};

struct winrt_listener {
    const struct winrt_listener_vtbl *lpVtbl;
    LONG refs;
    struct winrt_hub *hub;
    const IID *iid;
    enum winrt_listener_kind kind;
    uint32_t token;
};

struct winrt_listener_vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(struct winrt_listener *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(struct winrt_listener *self);
    ULONG(STDMETHODCALLTYPE *Release)(struct winrt_listener *self);
    HRESULT(STDMETHODCALLTYPE *Invoke)(struct winrt_listener *self, void *sender, void *args);
};

static ULONG STDMETHODCALLTYPE listener_add_ref(struct winrt_listener *self) {
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE listener_release(struct winrt_listener *self) {
    const LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        hub_release(self->hub);
        free(self);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE listener_query(struct winrt_listener *self, REFIID iid,
                                                void **out) {
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAgileObject) ||
        IsEqualIID(iid, self->iid)) {
        *out = self;
        (void)listener_add_ref(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* The bytes behind an IBuffer, which is only ever reached through IBufferByteAccess. */
static size_t buffer_copy(STREAMS(CIBuffer) * buffer, uint8_t *out, size_t capacity) {
    UINT32 len = 0;
    __x_Windows_CStorage_CStreams_CIBufferByteAccess *access = NULL;
    byte *bytes = NULL;
    if (buffer == NULL || FAILED(STREAMS(CIBuffer_get_Length)(buffer, &len)) ||
        FAILED(STREAMS(CIBuffer_QueryInterface)(
            buffer, &IID___x_Windows_CStorage_CStreams_CIBufferByteAccess, (void **)&access))) {
        return 0U;
    }
    size_t copied = 0U;
    if (SUCCEEDED(__x_Windows_CStorage_CStreams_CIBufferByteAccess_Buffer(access, &bytes)) &&
        bytes != NULL) {
        copied = len < capacity ? len : capacity;
        memcpy(out, bytes, copied);
    }
    (void)__x_Windows_CStorage_CStreams_CIBufferByteAccess_Release(access);
    return copied;
}

static void heard_merge_services(struct winrt_heard *entry, ADV(CIBluetoothLEAdvertisement) * ad) {
    __FIVector_1_GUID *uuids = NULL;
    if (FAILED(ADV(CIBluetoothLEAdvertisement_get_ServiceUuids)(ad, &uuids)) || uuids == NULL) {
        return;
    }
    unsigned count = 0;
    (void)__FIVector_1_GUID_get_Size(uuids, &count);
    for (unsigned i = 0; i < count; ++i) {
        GUID uuid;
        if (FAILED(__FIVector_1_GUID_GetAt(uuids, i, &uuid))) {
            continue;
        }
        bool known = false;
        for (size_t j = 0; j < entry->service_count && !known; ++j) {
            known = IsEqualGUID(&entry->services[j], &uuid);
        }
        if (!known && entry->service_count < WINRT_ADVERTISED_UUIDS) {
            entry->services[entry->service_count++] = uuid;
        }
    }
    (void)__FIVector_1_GUID_Release(uuids);
}

/* On the pool thread: into the heard table under the lock, and nothing else. An advertisement
   and its scan response arrive separately and only one carries the name, so a name is kept
   rather than overwritten with nothing, and service UUIDs accumulate. */
static void listener_advertisement(struct winrt_hub *hub,
                                   ADV(CIBluetoothLEAdvertisementReceivedEventArgs) * args) {
    UINT64 address = 0;
    INT16 rssi = 0;
    ADV(CIBluetoothLEAdvertisement) *ad = NULL;
    if (FAILED(ADV(CIBluetoothLEAdvertisementReceivedEventArgs_get_BluetoothAddress)(args,
                                                                                     &address)) ||
        FAILED(ADV(CIBluetoothLEAdvertisementReceivedEventArgs_get_Advertisement)(args, &ad))) {
        return;
    }
    (void)ADV(CIBluetoothLEAdvertisementReceivedEventArgs_get_RawSignalStrengthInDBm)(args, &rssi);
    char name[INKWELL_BLE_NAME_MAX] = {0};
    HSTRING local = NULL;
    if (SUCCEEDED(ADV(CIBluetoothLEAdvertisement_get_LocalName)(ad, &local)) && local != NULL) {
        UINT32 len = 0;
        const WCHAR *text = WindowsGetStringRawBuffer(local, &len);
        if (len > 0U) {
            (void)WideCharToMultiByte(CP_UTF8, 0, text, (int)len, name, (int)sizeof name - 1, NULL,
                                      NULL);
        }
        (void)WindowsDeleteString(local);
    }

    AcquireSRWLockExclusive(&hub->lock);
    struct winrt_heard *entry = NULL;
    for (size_t i = 0; i < hub->heard_count && entry == NULL; ++i) {
        entry = hub->heard[i].address == address ? &hub->heard[i] : NULL;
    }
    if (entry == NULL) {
        /* Full: the peripheral heard longest ago makes room. A table that only ever filled would
           stop admitting anything new once 64 addresses had passed - and a peripheral with a
           rotating private address is a new address every few minutes. */
        size_t slot = hub->heard_count;
        if (slot == WINRT_HEARD_MAX) {
            slot = 0U;
            for (size_t i = 1; i < WINRT_HEARD_MAX; ++i) {
                slot = hub->heard[i].heard_ms < hub->heard[slot].heard_ms ? i : slot;
            }
        } else {
            hub->heard_count += 1U;
        }
        entry = &hub->heard[slot];
        memset(entry, 0, sizeof *entry);
        entry->address = address;
    }
    {
        entry->rssi = rssi;
        entry->heard_ms = inkwell_time_monotonic_ms();
        if (name[0] != '\0') {
            inkwell_str_copy(entry->name, sizeof entry->name, name);
        }
        heard_merge_services(entry, ad);
    }
    ReleaseSRWLockExclusive(&hub->lock);
    (void)ADV(CIBluetoothLEAdvertisement_Release)(ad);
}

static void listener_value(struct winrt_listener *self, GATT(CIGattValueChangedEventArgs) * args) {
    STREAMS(CIBuffer) *value = NULL;
    if (FAILED(GATT(CIGattValueChangedEventArgs_get_CharacteristicValue)(args, &value))) {
        return;
    }
    struct winrt_event *event = calloc(1U, sizeof *event);
    if (event != NULL) {
        event->kind = WINRT_EVENT_NOTIFY;
        event->token = self->token;
        event->len = buffer_copy(value, event->data, sizeof event->data);
        hub_post(self->hub, event);
    }
    (void)STREAMS(CIBuffer_Release)(value);
}

static HRESULT STDMETHODCALLTYPE listener_invoke(struct winrt_listener *self, void *sender,
                                                 void *args) {
    (void)sender;
    if (args == NULL) {
        return S_OK;
    }
    if (self->kind == WINRT_LISTEN_ADVERTISEMENT) {
        listener_advertisement(self->hub, args);
    } else {
        listener_value(self, args);
    }
    return S_OK;
}

static const struct winrt_listener_vtbl kListenerVtbl = {
    listener_query,
    listener_add_ref,
    listener_release,
    listener_invoke,
};

static struct winrt_listener *listener_new(struct winrt_hub *hub, const IID *iid,
                                           enum winrt_listener_kind kind, uint32_t token) {
    struct winrt_listener *listener = calloc(1U, sizeof *listener);
    if (listener == NULL) {
        return NULL;
    }
    listener->lpVtbl = &kListenerVtbl;
    listener->refs = 1;
    listener->hub = hub;
    listener->iid = iid;
    listener->kind = kind;
    listener->token = token;
    hub_retain(hub);
    return listener;
}

/* ---- the loop thread's own state -------------------------------------------------------- */

struct winrt_characteristic {
    GUID uuid;
    GATT(CIGattCharacteristic) * characteristic;
    uint32_t properties;
};

/* One peripheral held open, which is what one central is for. */
struct winrt_link {
    bool active;
    uint64_t address;
    char address_text[INKWELL_BLE_ADDRESS_MAX];
    /* The connect this link belongs to: every operation started for it carries this token, and
       one that finishes after the link was replaced is recognised by it and dropped. */
    uint32_t generation;
    BT(CIBluetoothLEDevice) * device;
    GATT(CIGattSession) * session;
    GATT(CIGattDeviceService) * services[WINRT_SERVICES_MAX];
    size_t service_count;
    size_t services_pending;
    bool resolved;
    struct winrt_characteristic characteristics[WINRT_CHARACTERISTICS_MAX];
    size_t characteristic_count;
    /* The characteristic notifications come from, and its registration. `listener_token` is the
       subscribe that installed the listener; `notify_token` is set once that subscribe is
       confirmed, and is what a notification must carry to be delivered. */
    GATT(CIGattCharacteristic) * notifying;
    EventRegistrationToken value_registration;
    bool value_registered;
    uint32_t listener_token;
    uint32_t notify_token;
};

struct winrt_backend {
    struct winrt_hub *hub;
    bool wake_attached;
    bool ro_initialized;
    uint32_t next_token;
    /* 0 while the default adapter is being looked up, 1 when it is there, or a negative errno. */
    int adapter_state;
    BT(CIBluetoothAdapter) * adapter;
    ADV(CIBluetoothLEAdvertisementWatcher) * watcher;
    EventRegistrationToken received_registration;
    bool received_registered;
    struct winrt_link link;
};

static struct winrt_backend *backend_of(struct inkwell_ble_central *central) {
    return (struct winrt_backend *)central->backend;
}

static uint32_t next_token(struct winrt_backend *backend) {
    if (++backend->next_token == 0U) {
        backend->next_token = 1U;
    }
    return backend->next_token;
}

static HSTRING hstring_of(const WCHAR *text) {
    HSTRING out = NULL;
    (void)WindowsCreateString(text, (UINT32)wcslen(text), &out);
    return out;
}

/* Hands `op` to a new completion handler, which owns it from here whatever happens. */
static int start_op(struct winrt_backend *backend, void *op, const IID *handler_iid,
                    enum winrt_op kind, uint32_t token, size_t index) {
    if (op == NULL) {
        return -EIO;
    }
    struct winrt_completion *completion = calloc(1U, sizeof *completion);
    if (completion == NULL) {
        (void)((winrt_async *)op)->lpVtbl->Release(op);
        return -ENOMEM;
    }
    completion->lpVtbl = &kCompletionVtbl;
    completion->refs = 1;
    completion->hub = backend->hub;
    completion->iid = handler_iid;
    completion->op_kind = kind;
    completion->token = token;
    completion->index = index;
    completion->op = op;
    hub_retain(backend->hub);
    const HRESULT hr = completion->op->lpVtbl->put_Completed(completion->op, completion);
    (void)completion_release(completion);
    return SUCCEEDED(hr) ? 0 : -EIO;
}

static void post_answer(struct winrt_backend *backend, enum winrt_answer answer, uint32_t token,
                        size_t which, int result, bool value) {
    struct winrt_event *event = calloc(1U, sizeof *event);
    if (event == NULL) {
        return;
    }
    event->kind = WINRT_EVENT_ANSWER;
    event->answer = answer;
    event->token = token;
    event->which = which;
    event->result = result;
    event->value = value;
    hub_post(backend->hub, event);
}

/* ---- names ------------------------------------------------------------------------------ */

static void address_format(uint64_t address, char *out, size_t out_len) {
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                   (unsigned)((address >> 40U) & 0xFFU), (unsigned)((address >> 32U) & 0xFFU),
                   (unsigned)((address >> 24U) & 0xFFU), (unsigned)((address >> 16U) & 0xFFU),
                   (unsigned)((address >> 8U) & 0xFFU), (unsigned)(address & 0xFFU));
}

static bool address_parse(const char *text, uint64_t *out) {
    if (text == NULL || strlen(text) != 17U) {
        return false;
    }
    uint64_t value = 0U;
    for (size_t i = 0; i < 17U; ++i) {
        const char c = text[i];
        if (i % 3U == 2U) {
            if (c != ':') {
                return false;
            }
            continue;
        }
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        if (digit < 0) {
            return false;
        }
        value = (value << 4U) | (uint64_t)digit;
    }
    *out = value;
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* "6ba1b218-15a8-461f-9fa8-5dcae273eafd", as a caller spells a UUID, into a GUID. */
static bool uuid_parse(const char *text, GUID *out) {
    static const size_t kDashes[] = {8U, 13U, 18U, 23U};
    if (text == NULL || strlen(text) != 36U) {
        return false;
    }
    uint8_t bytes[16];
    size_t count = 0U;
    for (size_t i = 0; i < 36U;) {
        if (i == kDashes[0] || i == kDashes[1] || i == kDashes[2] || i == kDashes[3]) {
            if (text[i] != '-') {
                return false;
            }
            i += 1U;
            continue;
        }
        const int high = hex_value(text[i]);
        const int low = hex_value(text[i + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[count++] = (uint8_t)((high << 4) | low);
        i += 2U;
    }
    out->Data1 = ((unsigned long)bytes[0] << 24U) | ((unsigned long)bytes[1] << 16U) |
                 ((unsigned long)bytes[2] << 8U) | (unsigned long)bytes[3];
    out->Data2 = (unsigned short)((bytes[4] << 8U) | bytes[5]);
    out->Data3 = (unsigned short)((bytes[6] << 8U) | bytes[7]);
    memcpy(out->Data4, &bytes[8], 8U);
    return true;
}

/* A handle's address and characteristic, or false when it is not "<address>/<uuid>". */
static bool handle_split(const char *handle, uint64_t *address, GUID *uuid) {
    const char *slash = handle != NULL ? strrchr(handle, '/') : NULL;
    if (slash == NULL || (size_t)(slash - handle) >= INKWELL_BLE_ADDRESS_MAX) {
        return false;
    }
    char address_text[INKWELL_BLE_ADDRESS_MAX];
    memcpy(address_text, handle, (size_t)(slash - handle));
    address_text[slash - handle] = '\0';
    return address_parse(address_text, address) && uuid_parse(slash + 1, uuid);
}

/* ---- errors ----------------------------------------------------------------------------- */

/* An ATT error code (Core spec, Vol 3, Part F, 3.4.1.1) as the errno a caller acts on. */
static int att_to_errno(uint8_t code) {
    switch (code) {
    case 0x05: /* Insufficient Authentication */
    case 0x08: /* Insufficient Authorization */
    case 0x0C: /* Insufficient Encryption Key Size */
    case 0x0F: /* Insufficient Encryption */
        return -EACCES;
    case 0x01: /* Invalid Handle */
    case 0x0A: /* Attribute Not Found */
        return -ENOENT;
    case 0x0D: /* Invalid Attribute Value Length */
        return -EMSGSIZE;
    default:
        return -EIO;
    }
}

static int status_to_errno(GATT(CGattCommunicationStatus) status) {
    switch (status) {
    case GattCommunicationStatus_Success:
        return 0;
    case GattCommunicationStatus_Unreachable:
        return -ENOTCONN;
    case GattCommunicationStatus_AccessDenied:
        return -EACCES;
    case GattCommunicationStatus_ProtocolError:
    default:
        return -EIO;
    }
}

/* A write's or a descriptor write's outcome, with the ATT error when there was one. */
static int write_result_to_errno(GATT(CIGattWriteResult) * result) {
    GATT(CGattCommunicationStatus) status = GattCommunicationStatus_Unreachable;
    if (result == NULL || FAILED(GATT(CIGattWriteResult_get_Status)(result, &status))) {
        return -EIO;
    }
    if (status != GattCommunicationStatus_ProtocolError) {
        return status_to_errno(status);
    }
    __FIReference_1_BYTE *code = NULL;
    BYTE value = 0;
    int mapped = -EIO;
    if (SUCCEEDED(GATT(CIGattWriteResult_get_ProtocolError)(result, &code)) && code != NULL) {
        if (SUCCEEDED(__FIReference_1_BYTE_get_Value(code, &value))) {
            mapped = att_to_errno(value);
        }
        (void)__FIReference_1_BYTE_Release(code);
    }
    return mapped;
}

/* ---- the link --------------------------------------------------------------------------- */

static void release_closable(void *object) {
    if (object == NULL) {
        return;
    }
    __x_ABI_CWindows_CFoundation_CIClosable *closable = NULL;
    IUnknown *unknown = object;
    if (SUCCEEDED(IUnknown_QueryInterface(unknown, &IID___x_ABI_CWindows_CFoundation_CIClosable,
                                          (void **)&closable))) {
        (void)__x_ABI_CWindows_CFoundation_CIClosable_Close(closable);
        (void)__x_ABI_CWindows_CFoundation_CIClosable_Release(closable);
    }
    (void)IUnknown_Release(unknown);
}

static void link_unsubscribe(struct winrt_link *link) {
    if (link->notifying == NULL) {
        return;
    }
    if (link->value_registered) {
        (void)GATT(CIGattCharacteristic_remove_ValueChanged)(link->notifying,
                                                             link->value_registration);
        link->value_registered = false;
    }
    (void)GATT(CIGattCharacteristic_Release)(link->notifying);
    link->notifying = NULL;
    link->listener_token = 0U;
    link->notify_token = 0U;
}

/* Closing the device and its services is how an application lets go of a link on Windows; the
   stack drops it once nothing holds it open. */
static void link_close(struct winrt_link *link) {
    link_unsubscribe(link);
    for (size_t i = 0; i < link->characteristic_count; ++i) {
        (void)GATT(CIGattCharacteristic_Release)(link->characteristics[i].characteristic);
    }
    for (size_t i = 0; i < link->service_count; ++i) {
        release_closable(link->services[i]);
    }
    release_closable(link->session);
    release_closable(link->device);
    memset(link, 0, sizeof *link);
}

static struct winrt_link *link_for(struct winrt_backend *backend, uint64_t address) {
    return backend->link.active && backend->link.address == address ? &backend->link : NULL;
}

static bool link_connected(const struct winrt_link *link) {
    BT(CBluetoothConnectionStatus) status = BluetoothConnectionStatus_Disconnected;
    return link != NULL && link->device != NULL &&
           SUCCEEDED(BT(CIBluetoothLEDevice_get_ConnectionStatus)(link->device, &status)) &&
           status == BluetoothConnectionStatus_Connected;
}

static struct winrt_characteristic *characteristic_for(struct winrt_backend *backend,
                                                       const char *handle) {
    uint64_t address = 0U;
    GUID uuid;
    if (!handle_split(handle, &address, &uuid)) {
        return NULL;
    }
    struct winrt_link *link = link_for(backend, address);
    for (size_t i = 0; link != NULL && i < link->characteristic_count; ++i) {
        if (IsEqualGUID(&link->characteristics[i].uuid, &uuid)) {
            return &link->characteristics[i];
        }
    }
    return NULL;
}

/* -ENOTCONN for a handle on a link that has dropped, -ENOENT for one that was never there. */
static int missing_characteristic(struct winrt_backend *backend, const char *handle) {
    uint64_t address = 0U;
    GUID uuid;
    if (handle_split(handle, &address, &uuid)) {
        const struct winrt_link *link = link_for(backend, address);
        if (link != NULL && !link_connected(link)) {
            return -ENOTCONN;
        }
    }
    return -ENOENT;
}

/* ---- open and close --------------------------------------------------------------------- */

static void watcher_open(struct winrt_backend *backend) {
    HSTRING name = hstring_of(kWatcherClass);
    IInspectable *inspectable = NULL;
    const HRESULT activated = RoActivateInstance(name, &inspectable);
    (void)WindowsDeleteString(name);
    if (FAILED(activated) || FAILED(IInspectable_QueryInterface(inspectable, &IID_winrt_watcher,
                                                                (void **)&backend->watcher))) {
        if (inspectable != NULL) {
            (void)IInspectable_Release(inspectable);
        }
        backend->watcher = NULL;
        return;
    }
    (void)IInspectable_Release(inspectable);
    /* Active, for the scan response: it is what carries a peripheral's name. */
    (void)ADV(CIBluetoothLEAdvertisementWatcher_put_ScanningMode)(backend->watcher,
                                                                  BluetoothLEScanningMode_Active);
    struct winrt_listener *listener =
        listener_new(backend->hub, &IID_winrt_received_handler, WINRT_LISTEN_ADVERTISEMENT, 0U);
    if (listener != NULL) {
        backend->received_registered =
            SUCCEEDED(ADV(CIBluetoothLEAdvertisementWatcher_add_Received)(
                backend->watcher, (void *)listener, &backend->received_registration));
        (void)listener_release(listener);
    }
}

static void adapter_lookup(struct winrt_backend *backend) {
    HSTRING name = hstring_of(RuntimeClass_Windows_Devices_Bluetooth_BluetoothAdapter);
    BT(CIBluetoothAdapterStatics) *statics = NULL;
    const HRESULT got = RoGetActivationFactory(
        name, &IID___x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothAdapterStatics,
        (void **)&statics);
    (void)WindowsDeleteString(name);
    if (FAILED(got)) {
        backend->adapter_state = -ENODEV;
        return;
    }
    winrt_adapter_op *op = NULL;
    const HRESULT started = BT(CIBluetoothAdapterStatics_GetDefaultAsync)(statics, &op);
    (void)BT(CIBluetoothAdapterStatics_Release)(statics);
    if (FAILED(started) ||
        start_op(backend, op, &IID_winrt_adapter_done, WINRT_OP_ADAPTER, 0U, 0U) < 0) {
        backend->adapter_state = -ENODEV;
    }
}

int inkwell_ble_backend_open(struct inkwell_ble_central *central, bool private_connection,
                             const char *bus_address) {
    (void)private_connection;
    if (bus_address != NULL) {
        return -ENOSYS; /* a D-Bus seam; there is no bus here */
    }
    struct winrt_backend *backend = calloc(1U, sizeof *backend);
    struct winrt_hub *hub = calloc(1U, sizeof *hub);
    if (backend == NULL || hub == NULL) {
        free(backend);
        free(hub);
        return -ENOMEM;
    }
    /* The multithreaded apartment: WinRT then calls back on its own pool rather than waiting for
       a message pump the loop does not run. A thread already in a single-threaded apartment (a
       UI library got there first) answers RPC_E_CHANGED_MODE, which is not ours to undo; the
       Bluetooth objects are agile, so it works from there too. */
    const HRESULT ro = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(ro) && ro != RPC_E_CHANGED_MODE) {
        free(backend);
        free(hub);
        return -ENOSYS;
    }
    backend->ro_initialized = SUCCEEDED(ro);
    hub->refs = 1;
    InitializeSRWLock(&hub->lock);
    const int woke = inkwell_wake_open(&hub->wake);
    if (woke < 0) {
        if (backend->ro_initialized) {
            RoUninitialize();
        }
        free(backend);
        free(hub);
        return woke;
    }
    backend->hub = hub;
    central->backend = backend;
    adapter_lookup(backend);
    watcher_open(backend);
    return 0;
}

void inkwell_ble_backend_close(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    if (backend == NULL) {
        return;
    }
    struct winrt_hub *hub = backend->hub;
    if (backend->watcher != NULL) {
        (void)ADV(CIBluetoothLEAdvertisementWatcher_Stop)(backend->watcher);
        if (backend->received_registered) {
            (void)ADV(CIBluetoothLEAdvertisementWatcher_remove_Received)(
                backend->watcher, backend->received_registration);
        }
        (void)ADV(CIBluetoothLEAdvertisementWatcher_Release)(backend->watcher);
    }
    link_close(&backend->link);
    if (backend->adapter != NULL) {
        (void)BT(CIBluetoothAdapter_Release)(backend->adapter);
    }

    /* From here no handler posts or knocks: it finds the hub closed. */
    AcquireSRWLockExclusive(&hub->lock);
    hub->closed = true;
    struct winrt_event *event = hub->head;
    hub->head = NULL;
    hub->tail = NULL;
    ReleaseSRWLockExclusive(&hub->lock);
    while (event != NULL) {
        struct winrt_event *next = event->next;
        if (event->completion != NULL) {
            (void)completion_release(event->completion);
        }
        free(event);
        event = next;
    }
    if (backend->wake_attached && central->loop != NULL) {
        inkwell_loop_remove_fd(central->loop, hub->wake.fd);
    }
    inkwell_wake_close(&hub->wake);
    hub_release(hub);
    if (backend->ro_initialized) {
        RoUninitialize();
    }
    free(backend);
    central->backend = NULL;
}

/* ---- the adapter and the scan ----------------------------------------------------------- */

int inkwell_ble_backend_check_ready(struct inkwell_ble_central *central) {
    const int state = backend_of(central)->adapter_state;
    return state == 0 ? -EAGAIN : state < 0 ? state : 0;
}

int inkwell_ble_backend_find_adapter(struct inkwell_ble_central *central, char *name,
                                     size_t name_len) {
    struct winrt_backend *backend = backend_of(central);
    const int ready = inkwell_ble_backend_check_ready(central);
    if (ready < 0) {
        return ready == -EAGAIN ? -ENODEV : ready;
    }
    UINT64 address = 0;
    char text[INKWELL_BLE_ADDRESS_MAX] = "default";
    if (SUCCEEDED(BT(CIBluetoothAdapter_get_BluetoothAddress)(backend->adapter, &address))) {
        address_format(address, text, sizeof text);
    }
    inkwell_str_copy(central->adapter, sizeof central->adapter, text);
    inkwell_str_copy(name, name_len, text);
    return 0;
}

int inkwell_ble_backend_discovery(struct inkwell_ble_central *central, bool on) {
    struct winrt_backend *backend = backend_of(central);
    if (backend->watcher == NULL || backend->adapter_state < 0) {
        return -ENODEV;
    }
    const HRESULT hr = on ? ADV(CIBluetoothLEAdvertisementWatcher_Start)(backend->watcher)
                          : ADV(CIBluetoothLEAdvertisementWatcher_Stop)(backend->watcher);
    return SUCCEEDED(hr) ? 0 : -EIO;
}

/* The watcher's own status. A start that Windows refuses (Bluetooth is off) settles into
   Aborted rather than failing the call, and reads here as not scanning. */
int inkwell_ble_backend_discovering(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    ADV(CBluetoothLEAdvertisementWatcherStatus)
    status = BluetoothLEAdvertisementWatcherStatus_Stopped;
    if (backend->watcher == NULL ||
        FAILED(ADV(CIBluetoothLEAdvertisementWatcher_get_Status)(backend->watcher, &status))) {
        return 0;
    }
    return status == BluetoothLEAdvertisementWatcherStatus_Started ? 1 : 0;
}

int inkwell_ble_backend_list_by_service(struct inkwell_ble_central *central,
                                        const char *service_uuid,
                                        struct inkwell_ble_device *devices, size_t capacity,
                                        size_t *count) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_hub *hub = backend->hub;
    GUID wanted;
    *count = 0U;
    if (!uuid_parse(service_uuid, &wanted)) {
        return 0;
    }
    const uint64_t now = inkwell_time_monotonic_ms();
    const struct winrt_link *link = backend->link.active ? &backend->link : NULL;
    const bool link_up = link_connected(link);
    size_t matched = 0U;
    AcquireSRWLockShared(&hub->lock);
    for (size_t i = 0; i < hub->heard_count && matched < capacity; ++i) {
        const struct winrt_heard *entry = &hub->heard[i];
        bool offers = false;
        for (size_t j = 0; j < entry->service_count && !offers; ++j) {
            offers = IsEqualGUID(&entry->services[j], &wanted);
        }
        if (!offers) {
            continue;
        }
        const bool connected = link_up && link->address == entry->address;
        struct inkwell_ble_device *device = &devices[matched++];
        memset(device, 0, sizeof *device);
        address_format(entry->address, device->address, sizeof device->address);
        inkwell_str_copy(device->name, sizeof device->name, entry->name);
        device->rssi = entry->rssi;
        device->in_range = connected || now - entry->heard_ms <= WINRT_IN_RANGE_MS;
        /* No pair step: see the top of this file. */
        device->paired = true;
    }
    ReleaseSRWLockShared(&hub->lock);
    *count = matched;
    return 0;
}

/* ---- connect ---------------------------------------------------------------------------- */

int inkwell_ble_backend_connect(struct inkwell_ble_central *central, const char *address,
                                uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    uint64_t value = 0U;
    if (!address_parse(address, &value)) {
        return -ENOENT;
    }
    const uint32_t ours = next_token(backend);
    struct winrt_link *existing = link_for(backend, value);
    if (existing != NULL && link_connected(existing) && existing->service_count > 0U) {
        /* Already up: answer as a fresh connect would, on the next turn. */
        post_answer(backend, WINRT_ANSWER_CONNECT, ours, 0U, 0, false);
        *token = ours;
        return 0;
    }
    link_close(&backend->link);

    HSTRING name = hstring_of(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice);
    BT(CIBluetoothLEDeviceStatics) *statics = NULL;
    const HRESULT got = RoGetActivationFactory(
        name, &IID___x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics,
        (void **)&statics);
    (void)WindowsDeleteString(name);
    if (FAILED(got)) {
        return -ENOSYS;
    }
    winrt_device_op *op = NULL;
    const HRESULT started =
        BT(CIBluetoothLEDeviceStatics_FromBluetoothAddressAsync)(statics, value, &op);
    (void)BT(CIBluetoothLEDeviceStatics_Release)(statics);
    if (FAILED(started)) {
        return -EIO;
    }
    backend->link.active = true;
    backend->link.address = value;
    backend->link.generation = ours;
    address_format(value, backend->link.address_text, sizeof backend->link.address_text);
    const int result = start_op(backend, op, &IID_winrt_device_done, WINRT_OP_DEVICE, ours, 0U);
    if (result < 0) {
        link_close(&backend->link);
        return result;
    }
    *token = ours;
    return 0;
}

void inkwell_ble_backend_connect_cancel(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    /* A link still coming up is let go; one already connected is the caller's to disconnect. */
    if (backend->link.active && backend->link.service_count == 0U) {
        link_close(&backend->link);
    }
}

int inkwell_ble_backend_disconnect(struct inkwell_ble_central *central, const char *address) {
    struct winrt_backend *backend = backend_of(central);
    uint64_t value = 0U;
    if (address_parse(address, &value) && link_for(backend, value) != NULL) {
        link_close(&backend->link);
    }
    return 0;
}

/* See the top of this file: no pair step, answered on the next turn as every reply is. */
int inkwell_ble_backend_pair(struct inkwell_ble_central *central, const char *address,
                             uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    uint64_t value = 0U;
    const uint32_t ours = next_token(backend);
    post_answer(backend, WINRT_ANSWER_PAIR, ours, 0U, address_parse(address, &value) ? 0 : -ENOENT,
                false);
    *token = ours;
    return 0;
}

void inkwell_ble_backend_pair_cancel(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_set_trusted(struct inkwell_ble_central *central, const char *address,
                                    bool trusted) {
    (void)central;
    (void)address;
    (void)trusted;
    return 0;
}

int inkwell_ble_backend_forget(struct inkwell_ble_central *central, const char *address) {
    (void)central;
    (void)address;
    return -ENOTSUP; /* a bond is Windows Settings' to remove */
}

int inkwell_ble_backend_agent_register(struct inkwell_ble_central *central) {
    (void)central;
    return 0;
}

void inkwell_ble_backend_agent_unregister(struct inkwell_ble_central *central) {
    (void)central;
}

int inkwell_ble_backend_agent_answer(struct inkwell_ble_central *central, bool accept,
                                     uint32_t passkey) {
    (void)central;
    (void)accept;
    (void)passkey;
    return -ENOENT;
}

int inkwell_ble_backend_query(struct inkwell_ble_central *central, const char *address,
                              size_t which, uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    uint64_t value = 0U;
    const struct winrt_link *link =
        address_parse(address, &value) ? link_for(backend, value) : NULL;
    const bool connected = link_connected(link);
    const uint32_t ours = next_token(backend);
    if (which == 1U) {
        /* Discovery on a link that has dropped will never finish; say so rather than "not yet",
           which a caller would wait out to its own deadline. */
        post_answer(backend, WINRT_ANSWER_QUERY, ours, which, connected ? 0 : -ENOTCONN,
                    connected && link->resolved);
    } else {
        post_answer(backend, WINRT_ANSWER_QUERY, ours, which, 0, connected);
    }
    *token = ours;
    return 0;
}

int inkwell_ble_backend_find_characteristic(struct inkwell_ble_central *central,
                                            const char *address, const char *char_uuid,
                                            char *out_handle, size_t out_len) {
    char handle[INKWELL_BLE_HANDLE_MAX];
    const int written = snprintf(handle, sizeof handle, "%s/%s", address, char_uuid);
    if (written <= 0 || (size_t)written >= sizeof handle) {
        return -ENAMETOOLONG;
    }
    if (characteristic_for(backend_of(central), handle) == NULL) {
        return -ENOENT;
    }
    inkwell_str_copy(out_handle, out_len, handle);
    return strlen(handle) < out_len ? 0 : -ENAMETOOLONG;
}

/* The session's MaxPduSize is the ATT MTU it negotiated; without a session there is no answer. */
int inkwell_ble_backend_mtu(struct inkwell_ble_central *central, const char *handle,
                            uint16_t *out_mtu) {
    struct winrt_backend *backend = backend_of(central);
    uint64_t address = 0U;
    GUID uuid;
    const struct winrt_link *link =
        handle_split(handle, &address, &uuid) ? link_for(backend, address) : NULL;
    UINT16 pdu = 0;
    if (link == NULL || link->session == NULL ||
        FAILED(GATT(CIGattSession_get_MaxPduSize)(link->session, &pdu)) || pdu == 0U) {
        return -ENOTSUP;
    }
    *out_mtu = pdu;
    return 0;
}

int inkwell_ble_backend_link_held(struct inkwell_ble_central *central, const char *address) {
    (void)central;
    (void)address;
    return -ENOTSUP;
}

int inkwell_ble_backend_reset_adapter(struct inkwell_ble_central *central) {
    (void)central;
    return -ENOTSUP;
}

int inkwell_ble_backend_request_connection_interval(
    struct inkwell_ble_central *central, const char *address,
    const struct inkwell_ble_connection_parameters *parameters) {
    (void)central;
    (void)address;
    (void)parameters;
    return -ENOTSUP;
}

/* ---- reads, writes, notifications ------------------------------------------------------- */

static STREAMS(CIBuffer) * buffer_of(const uint8_t *data, size_t len) {
    HSTRING name = hstring_of(RuntimeClass_Windows_Storage_Streams_DataWriter);
    IInspectable *inspectable = NULL;
    const HRESULT activated = RoActivateInstance(name, &inspectable);
    (void)WindowsDeleteString(name);
    STREAMS(CIDataWriter) *writer = NULL;
    if (FAILED(activated) ||
        FAILED(IInspectable_QueryInterface(
            inspectable, &IID___x_ABI_CWindows_CStorage_CStreams_CIDataWriter, (void **)&writer))) {
        if (inspectable != NULL) {
            (void)IInspectable_Release(inspectable);
        }
        return NULL;
    }
    (void)IInspectable_Release(inspectable);
    STREAMS(CIBuffer) *buffer = NULL;
    if (FAILED(STREAMS(CIDataWriter_WriteBytes)(writer, (UINT32)len, (BYTE *)data)) ||
        FAILED(STREAMS(CIDataWriter_DetachBuffer)(writer, &buffer))) {
        buffer = NULL;
    }
    (void)STREAMS(CIDataWriter_Release)(writer);
    return buffer;
}

int inkwell_ble_backend_write(struct inkwell_ble_central *central, const char *handle,
                              const uint8_t *data, size_t len, uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_characteristic *found = characteristic_for(backend, handle);
    if (found == NULL) {
        return missing_characteristic(backend, handle);
    }
    GATT(CIGattCharacteristic3) *characteristic = NULL;
    if (FAILED(GATT(CIGattCharacteristic_QueryInterface)(
            found->characteristic, &IID_winrt_characteristic3, (void **)&characteristic))) {
        return -ENOSYS;
    }
    STREAMS(CIBuffer) *buffer = buffer_of(data, len);
    winrt_write_op *op = NULL;
    const HRESULT started =
        buffer != NULL ? GATT(CIGattCharacteristic3_WriteValueWithResultAndOptionAsync)(
                             characteristic, buffer, GattWriteOption_WriteWithResponse, &op)
                       : E_OUTOFMEMORY;
    if (buffer != NULL) {
        (void)STREAMS(CIBuffer_Release)(buffer);
    }
    (void)GATT(CIGattCharacteristic3_Release)(characteristic);
    if (FAILED(started)) {
        return started == E_OUTOFMEMORY ? -ENOMEM : -EIO;
    }
    const uint32_t ours = next_token(backend);
    const int result = start_op(backend, op, &IID_winrt_write_done, WINRT_OP_WRITE, ours, 0U);
    if (result == 0) {
        *token = ours;
    }
    return result;
}

int inkwell_ble_backend_read(struct inkwell_ble_central *central, const char *handle,
                             uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_characteristic *found = characteristic_for(backend, handle);
    if (found == NULL) {
        return missing_characteristic(backend, handle);
    }
    winrt_read_op *op = NULL;
    /* Uncached: a value read from Windows' cache is not a read of the peripheral, and a
       characteristic that a peer drains by being read must be read from the source. */
    if (FAILED(GATT(CIGattCharacteristic_ReadValueWithCacheModeAsync)(
            found->characteristic, BluetoothCacheMode_Uncached, &op))) {
        return -EIO;
    }
    const uint32_t ours = next_token(backend);
    const int result = start_op(backend, op, &IID_winrt_read_done, WINRT_OP_READ, ours, 0U);
    if (result == 0) {
        *token = ours;
    }
    return result;
}

int inkwell_ble_backend_subscribe(struct inkwell_ble_central *central, const char *handle,
                                  uint32_t *token) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_characteristic *found = characteristic_for(backend, handle);
    if (found == NULL) {
        return missing_characteristic(backend, handle);
    }
    struct winrt_link *link = &backend->link;
    /* Notify when the characteristic offers it, Indicate otherwise. */
    const GATT(CGattClientCharacteristicConfigurationDescriptorValue) wanted =
        (found->properties & (uint32_t)GattCharacteristicProperties_Notify) != 0U
            ? GattClientCharacteristicConfigurationDescriptorValue_Notify
        : (found->properties & (uint32_t)GattCharacteristicProperties_Indicate) != 0U
            ? GattClientCharacteristicConfigurationDescriptorValue_Indicate
            : GattClientCharacteristicConfigurationDescriptorValue_None;
    if (wanted == GattClientCharacteristicConfigurationDescriptorValue_None) {
        return -ENOTSUP;
    }
    GATT(CIGattCharacteristic3) *characteristic = NULL;
    if (FAILED(GATT(CIGattCharacteristic_QueryInterface)(
            found->characteristic, &IID_winrt_characteristic3, (void **)&characteristic))) {
        return -ENOSYS;
    }
    const uint32_t ours = next_token(backend);

    /* The listener goes on before the descriptor write, so the first value cannot beat it. */
    link_unsubscribe(link);
    struct winrt_listener *listener =
        listener_new(backend->hub, &IID_winrt_value_handler, WINRT_LISTEN_VALUE, ours);
    if (listener == NULL) {
        (void)GATT(CIGattCharacteristic3_Release)(characteristic);
        return -ENOMEM;
    }
    link->value_registered = SUCCEEDED(GATT(CIGattCharacteristic_add_ValueChanged)(
        found->characteristic, (void *)listener, &link->value_registration));
    (void)listener_release(listener);
    if (!link->value_registered) {
        (void)GATT(CIGattCharacteristic3_Release)(characteristic);
        return -EIO;
    }
    link->notifying = found->characteristic;
    link->listener_token = ours;
    (void)GATT(CIGattCharacteristic_AddRef)(link->notifying);

    winrt_write_op *op = NULL;
    const HRESULT started =
        GATT(CIGattCharacteristic3_WriteClientCharacteristicConfigurationDescriptorWithResultAsync)(
            characteristic, wanted, &op);
    (void)GATT(CIGattCharacteristic3_Release)(characteristic);
    const int result = SUCCEEDED(started) ? start_op(backend, op, &IID_winrt_write_done,
                                                     WINRT_OP_SUBSCRIBE, ours, 0U)
                                          : -EIO;
    if (result < 0) {
        link_unsubscribe(link);
        return result;
    }
    *token = ours;
    return 0;
}

/* ---- the loop ---------------------------------------------------------------------------- */

static int wake_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    (void)inkwell_ble_process(userdata);
    return 0;
}

int inkwell_ble_backend_attach(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    if (backend->wake_attached) {
        return 0;
    }
    const int result = inkwell_loop_add_fd(central->loop, backend->hub->wake.fd, INKWELL_LOOP_IN,
                                           wake_ready, central);
    if (result == 0) {
        backend->wake_attached = true;
    }
    return result;
}

void inkwell_ble_backend_detach(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    if (backend->wake_attached) {
        inkwell_loop_remove_fd(central->loop, backend->hub->wake.fd);
        backend->wake_attached = false;
    }
}

/* What a finished operation handed back, or NULL when it failed or was cancelled. */
static void *op_results(struct winrt_completion *completion) {
    void *result = NULL;
    if (completion->status != Completed ||
        FAILED(completion->op->lpVtbl->GetResults(completion->op, &result))) {
        return NULL;
    }
    return result;
}

static void start_discovery_of(struct winrt_backend *backend, struct winrt_link *link) {
    link->services_pending = 0U;
    for (size_t i = 0; i < link->service_count; ++i) {
        GATT(CIGattDeviceService3) *service = NULL;
        winrt_characteristics_op *op = NULL;
        if (FAILED(GATT(CIGattDeviceService_QueryInterface)(link->services[i], &IID_winrt_service3,
                                                            (void **)&service))) {
            continue;
        }
        /* The services were just read uncached; their characteristics came with them. */
        const HRESULT started = GATT(CIGattDeviceService3_GetCharacteristicsWithCacheModeAsync)(
            service, BluetoothCacheMode_Cached, &op);
        (void)GATT(CIGattDeviceService3_Release)(service);
        if (SUCCEEDED(started) && start_op(backend, op, &IID_winrt_characteristics_done,
                                           WINRT_OP_CHARACTERISTICS, link->generation, i) == 0) {
            link->services_pending += 1U;
        }
    }
    link->resolved = link->services_pending == 0U;
}

/* A GattSession, held with MaintainConnection so Windows keeps the link up between operations -
   and the one place the negotiated MTU can be read. */
static void start_session(struct winrt_backend *backend, struct winrt_link *link) {
    HSTRING id = NULL;
    if (FAILED(BT(CIBluetoothLEDevice_get_DeviceId)(link->device, &id))) {
        return;
    }
    HSTRING ids = hstring_of(RuntimeClass_Windows_Devices_Bluetooth_BluetoothDeviceId);
    HSTRING sessions = hstring_of(kSessionClass);
    BT(CIBluetoothDeviceIdStatics) *id_statics = NULL;
    GATT(CIGattSessionStatics) *session_statics = NULL;
    BT(CIBluetoothDeviceId) *device_id = NULL;
    winrt_session_op *op = NULL;
    if (SUCCEEDED(RoGetActivationFactory(
            ids, &IID___x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothDeviceIdStatics,
            (void **)&id_statics)) &&
        SUCCEEDED(RoGetActivationFactory(sessions, &IID_winrt_session_statics,
                                         (void **)&session_statics)) &&
        SUCCEEDED(BT(CIBluetoothDeviceIdStatics_FromId)(id_statics, id, &device_id)) &&
        SUCCEEDED(GATT(CIGattSessionStatics_FromDeviceIdAsync)(session_statics, device_id, &op))) {
        (void)start_op(backend, op, &IID_winrt_session_done, WINRT_OP_SESSION, link->generation,
                       0U);
    }
    if (device_id != NULL) {
        (void)BT(CIBluetoothDeviceId_Release)(device_id);
    }
    if (session_statics != NULL) {
        (void)GATT(CIGattSessionStatics_Release)(session_statics);
    }
    if (id_statics != NULL) {
        (void)BT(CIBluetoothDeviceIdStatics_Release)(id_statics);
    }
    (void)WindowsDeleteString(sessions);
    (void)WindowsDeleteString(ids);
    (void)WindowsDeleteString(id);
}

static void connect_failed(struct inkwell_ble_central *central, uint32_t token, int result) {
    struct winrt_backend *backend = backend_of(central);
    if (backend->link.generation == token) {
        link_close(&backend->link);
    }
    if (central->connect_state == 1 && central->connect_token == token) {
        inkwell_ble_connect_finish(central, result);
    }
}

static void apply_device(struct inkwell_ble_central *central, struct winrt_completion *done) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_link *link = &backend->link;
    BT(CIBluetoothLEDevice) *device = op_results(done);
    if (!link->active || link->generation != done->token) {
        if (device != NULL) {
            release_closable(device);
        }
        return;
    }
    if (device == NULL) {
        /* Windows has never heard of it: not in range, or an address that is not a device. */
        connect_failed(central, done->token, -ENOENT);
        return;
    }
    link->device = device;
    winrt_le_device3 *device3 = NULL;
    void *op = NULL;
    /* Reading the services uncached is what makes Windows connect; there is no connect call. */
    if (FAILED(BT(CIBluetoothLEDevice_QueryInterface)(device, &IID_winrt_IBluetoothLEDevice3,
                                                      (void **)&device3)) ||
        FAILED(device3->lpVtbl->GetGattServicesWithCacheModeAsync(
            device3, BluetoothCacheMode_Uncached, &op)) ||
        start_op(backend, op, &IID_winrt_services_done, WINRT_OP_SERVICES, done->token, 0U) < 0) {
        if (device3 != NULL) {
            (void)device3->lpVtbl->Release(device3);
        }
        connect_failed(central, done->token, -EIO);
        return;
    }
    (void)device3->lpVtbl->Release(device3);
    start_session(backend, link);
}

static void apply_services(struct inkwell_ble_central *central, struct winrt_completion *done) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_link *link = &backend->link;
    GATT(CIGattDeviceServicesResult) *result = op_results(done);
    if (!link->active || link->generation != done->token) {
        if (result != NULL) {
            (void)GATT(CIGattDeviceServicesResult_Release)(result);
        }
        return;
    }
    GATT(CGattCommunicationStatus) status = GattCommunicationStatus_Unreachable;
    winrt_services_view *services = NULL;
    if (result == NULL || FAILED(GATT(CIGattDeviceServicesResult_get_Status)(result, &status)) ||
        status != GattCommunicationStatus_Success ||
        FAILED(GATT(CIGattDeviceServicesResult_get_Services)(result, &services))) {
        if (result != NULL) {
            (void)GATT(CIGattDeviceServicesResult_Release)(result);
        }
        /* Unreachable is a peripheral that did not answer the connection. */
        connect_failed(central, done->token,
                       status == GattCommunicationStatus_Success ? -EIO : status_to_errno(status));
        return;
    }
    unsigned count = 0;
    (void)winrt_services_size(services, &count);
    for (unsigned i = 0; i < count && link->service_count < WINRT_SERVICES_MAX; ++i) {
        GATT(CIGattDeviceService) *service = NULL;
        if (SUCCEEDED(winrt_services_at(services, i, &service)) && service != NULL) {
            link->services[link->service_count++] = service;
        }
    }
    (void)winrt_services_release(services);
    (void)GATT(CIGattDeviceServicesResult_Release)(result);
    if (link->service_count == 0U) {
        connect_failed(central, done->token, -ENOENT);
        return;
    }
    if (central->connect_state == 1 && central->connect_token == done->token) {
        inkwell_ble_connect_finish(central, 0);
    }
    start_discovery_of(backend, link);
}

static void apply_characteristics(struct inkwell_ble_central *central,
                                  struct winrt_completion *done) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_link *link = &backend->link;
    GATT(CIGattCharacteristicsResult) *result = op_results(done);
    if (!link->active || link->generation != done->token) {
        if (result != NULL) {
            (void)GATT(CIGattCharacteristicsResult_Release)(result);
        }
        return;
    }
    winrt_characteristics_view *list = NULL;
    if (result != NULL &&
        SUCCEEDED(GATT(CIGattCharacteristicsResult_get_Characteristics)(result, &list)) &&
        list != NULL) {
        unsigned count = 0;
        (void)winrt_characteristics_size(list, &count);
        for (unsigned i = 0; i < count && link->characteristic_count < WINRT_CHARACTERISTICS_MAX;
             ++i) {
            struct winrt_characteristic *slot = &link->characteristics[link->characteristic_count];
            GATT(CGattCharacteristicProperties) properties = GattCharacteristicProperties_None;
            if (FAILED(winrt_characteristics_at(list, i, &slot->characteristic)) ||
                slot->characteristic == NULL) {
                continue;
            }
            (void)GATT(CIGattCharacteristic_get_Uuid)(slot->characteristic, &slot->uuid);
            (void)GATT(CIGattCharacteristic_get_CharacteristicProperties)(slot->characteristic,
                                                                          &properties);
            slot->properties = (uint32_t)properties;
            link->characteristic_count += 1U;
        }
        (void)winrt_characteristics_release(list);
    }
    if (result != NULL) {
        (void)GATT(CIGattCharacteristicsResult_Release)(result);
    }
    if (link->services_pending > 0U) {
        link->services_pending -= 1U;
    }
    link->resolved = link->services_pending == 0U;
}

static void apply_session(struct inkwell_ble_central *central, struct winrt_completion *done) {
    struct winrt_link *link = &backend_of(central)->link;
    GATT(CIGattSession) *session = op_results(done);
    if (session == NULL) {
        return;
    }
    if (!link->active || link->generation != done->token || link->session != NULL) {
        release_closable(session);
        return;
    }
    (void)GATT(CIGattSession_put_MaintainConnection)(session, TRUE);
    link->session = session;
}

static void apply_write(struct inkwell_ble_central *central, struct winrt_completion *done) {
    GATT(CIGattWriteResult) *result = op_results(done);
    const int mapped = result != NULL ? write_result_to_errno(result) : -EIO;
    if (result != NULL) {
        (void)GATT(CIGattWriteResult_Release)(result);
    }
    if (central->requests[0].state == 1 && central->requests[0].token == done->token) {
        inkwell_ble_pending_finish(&central->requests[0], mapped);
    }
}

static void apply_read(struct inkwell_ble_central *central, struct winrt_completion *done) {
    GATT(CIGattReadResult) *result = op_results(done);
    GATT(CGattCommunicationStatus) status = GattCommunicationStatus_Unreachable;
    STREAMS(CIBuffer) *value = NULL;
    int mapped = -EIO;
    size_t len = 0U;
    uint8_t bytes[INKWELL_BLE_VALUE_MAX];
    if (result != NULL && SUCCEEDED(GATT(CIGattReadResult_get_Status)(result, &status))) {
        mapped = status_to_errno(status);
        if (mapped == 0 && SUCCEEDED(GATT(CIGattReadResult_get_Value)(result, &value))) {
            len = buffer_copy(value, bytes, sizeof bytes);
            (void)STREAMS(CIBuffer_Release)(value);
        }
    }
    if (result != NULL) {
        (void)GATT(CIGattReadResult_Release)(result);
    }
    if (central->read_state == 1 && central->read_token == done->token) {
        if (mapped == 0) {
            memcpy(central->read_payload, bytes, len);
            central->read_length = len;
        }
        inkwell_ble_read_finish(central, mapped);
    }
}

static void apply_subscribe(struct inkwell_ble_central *central, struct winrt_completion *done) {
    struct winrt_link *link = &backend_of(central)->link;
    GATT(CIGattWriteResult) *result = op_results(done);
    const int mapped = result != NULL ? write_result_to_errno(result) : -EIO;
    if (result != NULL) {
        (void)GATT(CIGattWriteResult_Release)(result);
    }
    const bool current =
        central->requests[3].state == 1 && central->requests[3].token == done->token;
    /* Only the listener this subscribe installed is its to keep or remove. One that has since
       been replaced - the caller gave up on this subscribe and started another - belongs to the
       newer subscribe, and a late answer to the old one must not take it away. */
    const bool ours = link->notifying != NULL && link->listener_token == done->token;
    if (current && mapped == 0 && ours) {
        link->notify_token = done->token;
    } else if (ours) {
        /* Refused, or confirmed after the central gave up on it: the caller was told it failed,
           so nothing may go on notifying behind its back. */
        link_unsubscribe(link);
    }
    if (current) {
        inkwell_ble_subscribe_finish(central, mapped);
    }
}

static void apply_adapter(struct inkwell_ble_central *central, struct winrt_completion *done) {
    struct winrt_backend *backend = backend_of(central);
    BT(CIBluetoothAdapter) *adapter = op_results(done);
    boolean low_energy = FALSE;
    if (adapter == NULL) {
        backend->adapter_state = -ENODEV;
        return;
    }
    if (FAILED(BT(CIBluetoothAdapter_get_IsLowEnergySupported)(adapter, &low_energy)) ||
        !low_energy) {
        (void)BT(CIBluetoothAdapter_Release)(adapter);
        backend->adapter_state = -ENODEV;
        return;
    }
    backend->adapter = adapter;
    backend->adapter_state = 1;
}

static void apply_done(struct inkwell_ble_central *central, struct winrt_completion *done) {
    switch (done->op_kind) {
    case WINRT_OP_ADAPTER:
        apply_adapter(central, done);
        break;
    case WINRT_OP_DEVICE:
        apply_device(central, done);
        break;
    case WINRT_OP_SERVICES:
        apply_services(central, done);
        break;
    case WINRT_OP_CHARACTERISTICS:
        apply_characteristics(central, done);
        break;
    case WINRT_OP_SESSION:
        apply_session(central, done);
        break;
    case WINRT_OP_WRITE:
        apply_write(central, done);
        break;
    case WINRT_OP_READ:
        apply_read(central, done);
        break;
    case WINRT_OP_SUBSCRIBE:
        apply_subscribe(central, done);
        break;
    }
}

static void apply_answer(struct inkwell_ble_central *central, const struct winrt_event *event) {
    switch (event->answer) {
    case WINRT_ANSWER_CONNECT:
        if (central->connect_state == 1 && central->connect_token == event->token) {
            inkwell_ble_connect_finish(central, event->result);
        }
        break;
    case WINRT_ANSWER_PAIR:
        if (central->pair_state == 1 && central->pair_token == event->token) {
            inkwell_ble_pair_finish(central, event->result);
        }
        break;
    case WINRT_ANSWER_QUERY:
        if (event->which < 3U && central->requests[event->which].state == 1 &&
            central->requests[event->which].token == event->token) {
            central->requests[event->which].value = event->value;
            inkwell_ble_pending_finish(&central->requests[event->which], event->result);
        }
        break;
    }
}

int inkwell_ble_backend_process(struct inkwell_ble_central *central) {
    struct winrt_backend *backend = backend_of(central);
    struct winrt_hub *hub = backend->hub;
    (void)inkwell_wake_drain(&hub->wake);
    AcquireSRWLockExclusive(&hub->lock);
    struct winrt_event *event = hub->head;
    hub->head = NULL;
    hub->tail = NULL;
    ReleaseSRWLockExclusive(&hub->lock);

    while (event != NULL) {
        struct winrt_event *next = event->next;
        /* A completion can close the central (a caller giving up on a link): the rest is only
           released, never applied to a central that has gone. */
        const bool live = central->backend == backend;
        if (live && event->kind == WINRT_EVENT_DONE) {
            apply_done(central, event->completion);
        } else if (live && event->kind == WINRT_EVENT_ANSWER) {
            apply_answer(central, event);
        } else if (live && event->kind == WINRT_EVENT_NOTIFY) {
            /* Only from the subscription the central accepted. */
            if (central->notify_handle[0] != '\0' && backend->link.notify_token != 0U &&
                backend->link.notify_token == event->token) {
                inkwell_ble_notify(central, event->data, event->len);
            }
        }
        if (event->completion != NULL) {
            (void)completion_release(event->completion);
        }
        free(event);
        event = next;
    }
    return 0;
}
