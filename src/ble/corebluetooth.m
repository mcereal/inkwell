#include "central_internal.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/wake.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The CoreBluetooth backend.
 *
 * **The one place inkwell has a second thread, and it is not ours.** CoreBluetooth delivers
 * everything - a peripheral heard, a connect finished, a value read - as a delegate call on a
 * dispatch queue, and there is no descriptor to wait on instead. Draining the main queue from
 * the loop would need private API, so the manager gets a serial queue of its own and the rule
 * becomes: **nothing crosses between the two threads but copies.**
 *
 * - Every CoreBluetooth object, and every table about one, lives on that queue and is touched
 *   only from it.
 * - A delegate call does no work. It builds an event - a kind, a token, a result, a copy of the
 *   bytes - appends it to a list that also lives on the queue, and signals an inkwell wake.
 * - The loop thread owns the central. When the wake fires it takes the whole list in one short
 *   dispatch_sync, then applies each event with the same completion helpers the BlueZ backend
 *   calls. So a reply is matched against the central's tokens on the loop thread, exactly as a
 *   D-Bus reply is.
 * - A lookup the caller needs an answer to now (a listing, a characteristic, the MTU) is a
 *   dispatch_sync onto the queue. It waits for a table to be read, never for the radio.
 *
 * No lock is needed because nothing is shared: the queue serialises its side and the loop owns
 * the other. That includes the log, which may call an application's sink: a delegate's log line
 * is an event too, printed when the loop applies it.
 *
 * **Addresses are CoreBluetooth's identifiers.** macOS never shows a peripheral's hardware
 * address; each is named by an NSUUID that is stable on this Mac and meaningless on any other,
 * and that UUID's string is the address this backend hands out and accepts. A handle is
 * "<address>/<characteristic UUID>".
 *
 * **Bonding is the system's.** There is no agent: the first time an encrypted characteristic is
 * used, macOS raises its own pairing dialog, and the operation that needed it fails with an
 * authentication error while the user answers it. So every peripheral lists as `paired` - there
 * is no pair step for a caller to take - and pair_begin(), if asked anyway, finishes at once.
 */

/* ATT's own transaction timeout (Core spec, Vol 3, Part F, 3.3.3). CoreBluetooth encrypts a link
   the first time an encrypted characteristic is used and may hold a write until it has - or
   until the user has answered its pairing dialog - so the stack's own deadline is the one that
   means something here. */
const unsigned inkwell_ble_backend_write_timeout_ms = 30000U;

/* How long a peripheral stays "in range" after it was last heard. */
#define CB_IN_RANGE_MS 15000U
/* How long subscribe() waits for the stack to confirm. Bounded, like BlueZ's StartNotify: an
   encrypted characteristic answers at once with an error while macOS asks the user. */
#define CB_SUBSCRIBE_TIMEOUT_MS 8000U

enum cb_event_kind {
    CB_EVENT_CONNECT,
    CB_EVENT_PAIR,
    CB_EVENT_WRITE,
    CB_EVENT_READ,
    CB_EVENT_QUERY,
    CB_EVENT_NOTIFY,
    CB_EVENT_LOG_INFO, /* `data` is the line */
    CB_EVENT_LOG_WARN,
};

struct cb_event {
    enum cb_event_kind kind;
    uint32_t token;
    int result;
    size_t which; /* CB_EVENT_QUERY: the request index */
    bool value;
    size_t len;
    uint8_t data[INKWELL_BLE_VALUE_MAX];
};

/* ---- errors ------------------------------------------------------------------------------ */

static int error_to_errno(NSError *error) {
    if (error == nil) {
        return 0;
    }
    if ([error.domain isEqualToString:CBATTErrorDomain]) {
        switch (error.code) {
        case CBATTErrorInsufficientAuthentication:
        case CBATTErrorInsufficientAuthorization:
        case CBATTErrorInsufficientEncryption:
        case CBATTErrorInsufficientEncryptionKeySize:
            return -EACCES;
        case CBATTErrorInvalidHandle:
        case CBATTErrorAttributeNotFound:
            return -ENOENT;
        case CBATTErrorInvalidAttributeValueLength:
            return -EMSGSIZE;
        default:
            return -EIO;
        }
    }
    if ([error.domain isEqualToString:CBErrorDomain]) {
        switch (error.code) {
        case CBErrorConnectionTimeout:
        case CBErrorEncryptionTimedOut:
            return -ETIMEDOUT;
        case CBErrorNotConnected:
        case CBErrorPeripheralDisconnected:
            return -ENOTCONN;
        case CBErrorPeerRemovedPairingInformation:
        case CBErrorUUIDNotAllowed:
            return -EACCES;
        case CBErrorOperationCancelled:
            return -ECANCELED;
        case CBErrorConnectionLimitReached:
            return -EBUSY;
        default:
            return -EIO;
        }
    }
    return -EIO;
}

/* ---- what the queue knows ---------------------------------------------------------------- */

/* A peripheral heard in a scan. */
@interface IWSeen : NSObject
@property(nonatomic, strong) CBPeripheral *peripheral;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) int16_t rssi;
@property(nonatomic) uint64_t heardMs;
@property(nonatomic, strong) NSMutableSet<CBUUID *> *services;
@end

@implementation IWSeen
@end

/* A peripheral a connect was asked for, and everything outstanding on it. */
@interface IWLink : NSObject
@property(nonatomic, strong) CBPeripheral *peripheral;
@property(nonatomic) uint32_t connectToken;
@property(nonatomic) BOOL resolved;
@property(nonatomic) NSInteger servicesPending;
/* The one write and the one read in flight, and what they were on. */
@property(nonatomic) uint32_t writeToken;
@property(nonatomic, strong) CBCharacteristic *writeCharacteristic;
@property(nonatomic) uint32_t readToken;
@property(nonatomic, strong) CBCharacteristic *readCharacteristic;
@end

@implementation IWLink
@end

@interface IWCentral : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) CBCentralManager *manager;
@property(nonatomic, strong) NSMutableDictionary<NSUUID *, IWSeen *> *seen;
@property(nonatomic, strong) NSMutableDictionary<NSUUID *, IWLink *> *links;
@property(nonatomic, strong) NSMutableArray<NSData *> *events;
@property(nonatomic, strong) CBCharacteristic *notifying;
@property(nonatomic, strong) dispatch_semaphore_t subscribeDone;
@property(nonatomic) int subscribeResult;
@property(nonatomic) BOOL scanning;
@property(nonatomic) BOOL closed;
@property(nonatomic) int wakeFd;
@end

@implementation IWCentral

- (void)post:(const struct cb_event *)event {
    if (self.closed) {
        return;
    }
    const size_t size = offsetof(struct cb_event, data) + event->len;
    [self.events addObject:[NSData dataWithBytes:event length:size]];
    /* A byte on the wake's write end. Several events before the loop looks are one look. */
    const uint8_t one = 1U;
    (void)write(self.wakeFd, &one, sizeof one);
}

/* A log line from the queue, printed on the loop thread. */
- (void)log:(enum cb_event_kind)kind what:(const char *)what error:(NSError *)error {
    struct cb_event event;
    memset(&event, 0, offsetof(struct cb_event, data));
    event.kind = kind;
    const char *reason = error.localizedDescription.UTF8String;
    const int written = snprintf((char *)event.data, sizeof event.data, "%s: %s", what,
                                 reason != NULL ? reason : "no reason given");
    event.len = written < 0 ? 0U
                            : ((size_t)written < sizeof event.data ? (size_t)written + 1U
                                                                   : sizeof event.data);
    event.data[sizeof event.data - 1U] = '\0';
    [self post:&event];
}

- (void)postKind:(enum cb_event_kind)kind token:(uint32_t)token result:(int)result {
    if (token == 0U) {
        return;
    }
    struct cb_event event;
    memset(&event, 0, offsetof(struct cb_event, data));
    event.kind = kind;
    event.token = token;
    event.result = result;
    [self post:&event];
}

- (IWLink *)linkFor:(CBPeripheral *)peripheral {
    return self.links[peripheral.identifier];
}

/* Fails whatever was outstanding on a link that has gone. */
- (void)failLink:(IWLink *)link result:(int)result {
    [self postKind:CB_EVENT_CONNECT token:link.connectToken result:result];
    [self postKind:CB_EVENT_WRITE token:link.writeToken result:result];
    [self postKind:CB_EVENT_READ token:link.readToken result:result];
    link.connectToken = 0U;
    link.writeToken = 0U;
    link.readToken = 0U;
    link.writeCharacteristic = nil;
    link.readCharacteristic = nil;
    link.resolved = NO;
}

/* ---- CBCentralManagerDelegate ---- */

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state != CBManagerStatePoweredOn) {
        self.scanning = NO;
        for (IWLink *link in self.links.allValues) {
            [self failLink:link result:-ENOTCONN];
        }
    }
    /* Nothing is waiting on this directly; check_ready() reads the state when asked. The wake
       is so a caller parked on "not ready" hears about it on its next turn. */
    const uint8_t one = 1U;
    (void)write(self.wakeFd, &one, sizeof one);
}

- (void)centralManager:(CBCentralManager *)central
    didDiscoverPeripheral:(CBPeripheral *)peripheral
        advertisementData:(NSDictionary<NSString *, id> *)advertisement
                     RSSI:(NSNumber *)RSSI {
    (void)central;
    IWSeen *seen = self.seen[peripheral.identifier];
    if (seen == nil) {
        seen = [IWSeen new];
        seen.services = [NSMutableSet set];
        self.seen[peripheral.identifier] = seen;
    }
    seen.peripheral = peripheral;
    NSString *name = advertisement[CBAdvertisementDataLocalNameKey];
    if (name.length == 0U) {
        name = peripheral.name;
    }
    if (name.length > 0U) {
        seen.name = name;
    }
    /* 127 is CoreBluetooth's "no reading". */
    if (RSSI != nil && RSSI.intValue != 127) {
        seen.rssi = (int16_t)RSSI.intValue;
    }
    seen.heardMs = inkwell_time_monotonic_ms();
    NSArray<CBUUID *> *advertised = advertisement[CBAdvertisementDataServiceUUIDsKey];
    if (advertised != nil) {
        [seen.services addObjectsFromArray:advertised];
    }
    NSArray<CBUUID *> *overflow = advertisement[CBAdvertisementDataOverflowServiceUUIDsKey];
    if (overflow != nil) {
        [seen.services addObjectsFromArray:overflow];
    }
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral {
    (void)central;
    IWLink *link = [self linkFor:peripheral];
    if (link == nil) {
        return;
    }
    peripheral.delegate = self;
    link.resolved = NO;
    link.servicesPending = 0;
    [peripheral discoverServices:nil];
    [self postKind:CB_EVENT_CONNECT token:link.connectToken result:0];
    link.connectToken = 0U;
}

- (void)centralManager:(CBCentralManager *)central
    didFailToConnectPeripheral:(CBPeripheral *)peripheral
                         error:(NSError *)error {
    (void)central;
    IWLink *link = [self linkFor:peripheral];
    if (link == nil) {
        return;
    }
    const int result = error != nil ? error_to_errno(error) : -EIO;
    [self log:CB_EVENT_LOG_WARN what:"Connect failed" error:error];
    [self failLink:link result:result];
}

- (void)centralManager:(CBCentralManager *)central
    didDisconnectPeripheral:(CBPeripheral *)peripheral
                      error:(NSError *)error {
    (void)central;
    IWLink *link = [self linkFor:peripheral];
    if (link == nil) {
        return;
    }
    if (error != nil) {
        [self log:CB_EVENT_LOG_INFO what:"Link dropped" error:error];
    }
    [self failLink:link result:-ENOTCONN];
    if (self.notifying != nil && self.notifying.service.peripheral == peripheral) {
        self.notifying = nil;
    }
}

/* ---- CBPeripheralDelegate ---- */

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    IWLink *link = [self linkFor:peripheral];
    if (link == nil) {
        return;
    }
    if (error != nil) {
        [self log:CB_EVENT_LOG_WARN what:"Service discovery failed" error:error];
        return;
    }
    link.servicesPending = (NSInteger)peripheral.services.count;
    if (link.servicesPending == 0) {
        link.resolved = YES;
        return;
    }
    for (CBService *service in peripheral.services) {
        [peripheral discoverCharacteristics:nil forService:service];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didDiscoverCharacteristicsForService:(CBService *)service
                                   error:(NSError *)error {
    (void)service;
    IWLink *link = [self linkFor:peripheral];
    if (link == nil) {
        return;
    }
    if (error != nil) {
        [self log:CB_EVENT_LOG_WARN what:"Characteristic discovery failed" error:error];
    }
    if (--link.servicesPending <= 0) {
        link.resolved = YES;
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
                             error:(NSError *)error {
    IWLink *link = [self linkFor:peripheral];
    if (link == nil || link.writeToken == 0U || link.writeCharacteristic != characteristic) {
        return;
    }
    [self postKind:CB_EVENT_WRITE token:link.writeToken result:error_to_errno(error)];
    link.writeToken = 0U;
    link.writeCharacteristic = nil;
}

/*
 * One callback for two things: the answer to a read, and a notification. A read pending on
 * this characteristic takes the value; otherwise it is a notification if this is the one
 * subscribed to, and nothing if it is not.
 */
- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
                              error:(NSError *)error {
    IWLink *link = [self linkFor:peripheral];
    NSData *value = characteristic.value;
    const size_t len = value.length > INKWELL_BLE_VALUE_MAX ? INKWELL_BLE_VALUE_MAX : value.length;
    if (link != nil && link.readToken != 0U && link.readCharacteristic == characteristic) {
        struct cb_event event;
        memset(&event, 0, offsetof(struct cb_event, data));
        event.kind = CB_EVENT_READ;
        event.token = link.readToken;
        event.result = error_to_errno(error);
        if (event.result == 0 && value.length > INKWELL_BLE_VALUE_MAX) {
            event.result = -EMSGSIZE;
        }
        if (event.result == 0) {
            event.len = len;
            memcpy(event.data, value.bytes, len);
        }
        link.readToken = 0U;
        link.readCharacteristic = nil;
        [self post:&event];
        return;
    }
    if (error == nil && characteristic == self.notifying && len > 0U) {
        struct cb_event event;
        memset(&event, 0, offsetof(struct cb_event, data));
        event.kind = CB_EVENT_NOTIFY;
        event.len = len;
        memcpy(event.data, value.bytes, len);
        [self post:&event];
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
                                          error:(NSError *)error {
    (void)peripheral;
    if (self.subscribeDone == nil) {
        return;
    }
    self.subscribeResult = error_to_errno(error);
    if (error != nil) {
        [self log:CB_EVENT_LOG_WARN what:"Subscribe refused" error:error];
    }
    if (self.subscribeResult == 0 && characteristic.isNotifying) {
        self.notifying = characteristic;
    }
    dispatch_semaphore_signal(self.subscribeDone);
}

/* ---- lookups, on the queue ---- */

- (CBPeripheral *)peripheralFor:(NSString *)address {
    NSUUID *identifier = [[NSUUID alloc] initWithUUIDString:address];
    if (identifier == nil) {
        return nil;
    }
    IWLink *link = self.links[identifier];
    if (link != nil) {
        return link.peripheral;
    }
    IWSeen *seen = self.seen[identifier];
    if (seen != nil) {
        return seen.peripheral;
    }
    /* A peripheral this Mac knows from an earlier run, heard or not. */
    NSArray<CBPeripheral *> *known =
        [self.manager retrievePeripheralsWithIdentifiers:@[ identifier ]];
    return known.firstObject;
}

- (CBCharacteristic *)characteristicOn:(CBPeripheral *)peripheral uuid:(NSString *)uuid {
    if (peripheral == nil || uuid.length == 0U) {
        return nil;
    }
    CBUUID *wanted = [CBUUID UUIDWithString:uuid];
    for (CBService *service in peripheral.services) {
        for (CBCharacteristic *characteristic in service.characteristics) {
            if ([characteristic.UUID isEqual:wanted]) {
                return characteristic;
            }
        }
    }
    return nil;
}

/* "<address>/<characteristic UUID>" back to the characteristic, or nil. */
- (CBCharacteristic *)characteristicFor:(NSString *)handle peripheral:(CBPeripheral **)out {
    const NSRange slash = [handle rangeOfString:@"/"];
    if (slash.location == NSNotFound) {
        return nil;
    }
    NSString *address = [handle substringToIndex:slash.location];
    NSString *uuid = [handle substringFromIndex:slash.location + 1U];
    CBPeripheral *peripheral = [self peripheralFor:address];
    if (out != NULL) {
        *out = peripheral;
    }
    if (peripheral == nil || peripheral.state != CBPeripheralStateConnected) {
        return nil;
    }
    @try {
        return [self characteristicOn:peripheral uuid:uuid];
    } @catch (NSException *exception) {
        (void)exception;
        return nil; /* not a UUID CBUUID will parse */
    }
}

@end

/* ---- the backend's C half, on the loop thread ------------------------------------------ */

struct cb_backend {
    void *object; /* IWCentral, retained */
    struct inkwell_wake wake;
    bool wake_attached;
    uint32_t next_token;
};

static struct cb_backend *backend_of(struct inkwell_ble_central *central) {
    return (struct cb_backend *)central->backend;
}

static IWCentral *object_of(struct inkwell_ble_central *central) {
    struct cb_backend *backend = backend_of(central);
    return backend != NULL ? (__bridge IWCentral *)backend->object : nil;
}

static uint32_t next_token(struct cb_backend *backend) {
    if (++backend->next_token == 0U) {
        backend->next_token = 1U;
    }
    return backend->next_token;
}

static NSString *string_of(const char *text) {
    return text != NULL ? [NSString stringWithUTF8String:text] : nil;
}

int inkwell_ble_backend_open(struct inkwell_ble_central *central, bool private_connection,
                             const char *bus_address) {
    (void)private_connection;
    if (bus_address != NULL) {
        return -ENOSYS; /* a D-Bus seam; there is no bus here */
    }
    struct cb_backend *backend = calloc(1U, sizeof *backend);
    if (backend == NULL) {
        return -ENOMEM;
    }
    const int woke = inkwell_wake_open(&backend->wake);
    if (woke < 0) {
        free(backend);
        return woke;
    }
    backend->next_token = 0U;

    @autoreleasepool {
        IWCentral *object = [IWCentral new];
        object.queue = dispatch_queue_create("org.inkwell.ble", DISPATCH_QUEUE_SERIAL);
        object.seen = [NSMutableDictionary dictionary];
        object.links = [NSMutableDictionary dictionary];
        object.events = [NSMutableArray array];
        object.wakeFd = backend->wake.write_fd;
        /* No power alert: the application says Bluetooth is off in its own words. */
        object.manager =
            [[CBCentralManager alloc] initWithDelegate:object
                                                 queue:object.queue
                                               options:@{
                                                   CBCentralManagerOptionShowPowerAlertKey : @NO
                                               }];
        backend->object = (__bridge_retained void *)object;
    }
    central->backend = backend;
    return 0;
}

void inkwell_ble_backend_close(struct inkwell_ble_central *central) {
    struct cb_backend *backend = backend_of(central);
    if (backend == NULL) {
        return;
    }
    @autoreleasepool {
        IWCentral *object = (__bridge_transfer IWCentral *)backend->object;
        backend->object = NULL;
        /* After this nothing on the queue posts or writes to the wake, which is about to go. */
        dispatch_sync(object.queue, ^{
          object.closed = YES;
          if (object.scanning) {
              [object.manager stopScan];
          }
          for (IWLink *link in object.links.allValues) {
              [object.manager cancelPeripheralConnection:link.peripheral];
          }
          object.manager.delegate = nil;
          [object.links removeAllObjects];
          [object.events removeAllObjects];
        });
    }
    if (backend->wake_attached && central->loop != NULL) {
        inkwell_loop_remove_fd(central->loop, backend->wake.fd);
    }
    inkwell_wake_close(&backend->wake);
    free(backend);
    central->backend = NULL;
}

int inkwell_ble_backend_check_ready(struct inkwell_ble_central *central) {
    IWCentral *object = object_of(central);
    __block CBManagerState state = CBManagerStateUnknown;
    dispatch_sync(object.queue, ^{
      state = object.manager.state;
    });
    switch (state) {
    case CBManagerStatePoweredOn:
        return 0;
    case CBManagerStateUnauthorized:
        return -EACCES;
    case CBManagerStateUnsupported:
    case CBManagerStatePoweredOff:
        return -ENODEV;
    case CBManagerStateUnknown:
    case CBManagerStateResetting:
    default:
        return -EAGAIN;
    }
}

int inkwell_ble_backend_find_adapter(struct inkwell_ble_central *central, char *name,
                                     size_t name_len) {
    const int ready = inkwell_ble_backend_check_ready(central);
    if (ready < 0) {
        return ready == -EAGAIN ? -ENODEV : ready;
    }
    inkwell_str_copy(central->adapter, sizeof central->adapter, "CoreBluetooth");
    inkwell_str_copy(name, name_len, "CoreBluetooth");
    return 0;
}

int inkwell_ble_backend_discovery(struct inkwell_ble_central *central, bool on) {
    IWCentral *object = object_of(central);
    __block int result = 0;
    dispatch_sync(object.queue, ^{
      if (object.manager.state != CBManagerStatePoweredOn) {
          result = -ENODEV;
          return;
      }
      if (on && !object.scanning) {
          /* Duplicates on: the RSSI and the "heard" time are what say a peripheral is in range,
             and both go stale without them. */
          [object.manager
              scanForPeripheralsWithServices:nil
                                     options:@{
                                         CBCentralManagerScanOptionAllowDuplicatesKey : @YES
                                     }];
          object.scanning = YES;
      } else if (!on && object.scanning) {
          [object.manager stopScan];
          object.scanning = NO;
      }
    });
    return result;
}

int inkwell_ble_backend_list_by_service(struct inkwell_ble_central *central,
                                        const char *service_uuid,
                                        struct inkwell_ble_device *devices, size_t capacity,
                                        size_t *count) {
    IWCentral *object = object_of(central);
    NSString *wanted_string = string_of(service_uuid);
    __block size_t matched = 0U;
    dispatch_sync(object.queue, ^{
      CBUUID *wanted = nil;
      @try {
          wanted = [CBUUID UUIDWithString:wanted_string];
      } @catch (NSException *exception) {
          (void)exception;
          return;
      }
      const uint64_t now = inkwell_time_monotonic_ms();
      for (IWSeen *seen in object.seen.allValues) {
          if (matched >= capacity) {
              break;
          }
          IWLink *link = object.links[seen.peripheral.identifier];
          const bool connected = link != nil && seen.peripheral.state == CBPeripheralStateConnected;
          if (![seen.services containsObject:wanted] && !connected) {
              continue;
          }
          struct inkwell_ble_device *device = &devices[matched++];
          memset(device, 0, sizeof *device);
          inkwell_str_copy(device->address, sizeof device->address,
                           seen.peripheral.identifier.UUIDString.UTF8String);
          if (seen.name.length > 0U) {
              inkwell_str_copy(device->name, sizeof device->name, seen.name.UTF8String);
          }
          device->rssi = seen.rssi;
          device->in_range = connected || now - seen.heardMs <= CB_IN_RANGE_MS;
          /* macOS bonds on demand and keeps its bonds to itself, so there is never a pair step
             for a caller to take - which is what `paired` tells one. See central.h. */
          device->paired = true;
      }
    });
    *count = matched;
    return 0;
}

int inkwell_ble_backend_connect(struct inkwell_ble_central *central, const char *address,
                                uint32_t *token) {
    IWCentral *object = object_of(central);
    const uint32_t ours = next_token(backend_of(central));
    NSString *address_string = string_of(address);
    __block int result = 0;
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = [object peripheralFor:address_string];
      if (peripheral == nil) {
          result = -ENOENT;
          return;
      }
      IWLink *link = object.links[peripheral.identifier];
      if (link == nil) {
          link = [IWLink new];
          link.peripheral = peripheral;
          object.links[peripheral.identifier] = link;
      }
      link.connectToken = ours;
      if (peripheral.state == CBPeripheralStateConnected) {
          /* Already up (another process's link counts): answer as a fresh connect would. */
          peripheral.delegate = object;
          [object postKind:CB_EVENT_CONNECT token:ours result:0];
          link.connectToken = 0U;
          if (!link.resolved && peripheral.services == nil) {
              [peripheral discoverServices:nil];
          }
          return;
      }
      [object.manager connectPeripheral:peripheral options:nil];
    });
    if (result == 0) {
        *token = ours;
    }
    return result;
}

void inkwell_ble_backend_connect_cancel(struct inkwell_ble_central *central) {
    IWCentral *object = object_of(central);
    dispatch_sync(object.queue, ^{
      for (IWLink *link in object.links.allValues) {
          if (link.connectToken != 0U) {
              link.connectToken = 0U;
              [object.manager cancelPeripheralConnection:link.peripheral];
          }
      }
    });
}

int inkwell_ble_backend_disconnect(struct inkwell_ble_central *central, const char *address) {
    IWCentral *object = object_of(central);
    NSString *address_string = string_of(address);
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = [object peripheralFor:address_string];
      if (peripheral == nil) {
          return;
      }
      if (object.notifying != nil && object.notifying.service.peripheral == peripheral) {
          [peripheral setNotifyValue:NO forCharacteristic:object.notifying];
          object.notifying = nil;
      }
      [object.manager cancelPeripheralConnection:peripheral];
      IWLink *link = object.links[peripheral.identifier];
      if (link != nil) {
          [object failLink:link result:-ENOTCONN];
      }
    });
    return 0;
}

int inkwell_ble_backend_pair(struct inkwell_ble_central *central, const char *address,
                             uint32_t *token) {
    NSString *address_string = string_of(address);
    /* macOS bonds on demand, with its own dialog; there is nothing to start. The answer still
       arrives on a later turn, as every other reply does. */
    IWCentral *object = object_of(central);
    const uint32_t ours = next_token(backend_of(central));
    dispatch_async(object.queue, ^{
      NSUUID *identifier = [[NSUUID alloc] initWithUUIDString:address_string];
      [object postKind:CB_EVENT_PAIR token:ours result:identifier != nil ? 0 : -ENOENT];
    });
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
    return -ENOTSUP; /* only System Settings can forget a device on macOS */
}

/* There is no agent: macOS asks the user itself. Registering one succeeds so a caller does not
   report pairing as unavailable, and no request is ever pending. */
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
    IWCentral *object = object_of(central);
    const uint32_t ours = next_token(backend_of(central));
    NSString *address_string = string_of(address);
    dispatch_async(object.queue, ^{
      CBPeripheral *peripheral = [object peripheralFor:address_string];
      IWLink *link = peripheral != nil ? object.links[peripheral.identifier] : nil;
      const bool connected = peripheral != nil && peripheral.state == CBPeripheralStateConnected;
      struct cb_event event;
      memset(&event, 0, offsetof(struct cb_event, data));
      event.kind = CB_EVENT_QUERY;
      event.token = ours;
      event.which = which;
      event.value = which == 1U ? (connected && link != nil && link.resolved) : connected;
      /* Discovery on a link that has already dropped will never finish; say so rather than
         "not yet", which a caller would wait out to its own deadline. The connected query is the
         one whose answer is "no". */
      if (which == 1U && !connected) {
          event.result = -ENOTCONN;
      }
      [object post:&event];
    });
    *token = ours;
    return 0;
}

int inkwell_ble_backend_find_characteristic(struct inkwell_ble_central *central,
                                            const char *address, const char *char_uuid,
                                            char *out_handle, size_t out_len) {
    IWCentral *object = object_of(central);
    NSString *address_string = string_of(address);
    NSString *uuid_string = string_of(char_uuid);
    __block bool found = false;
    dispatch_sync(object.queue, ^{
      @try {
          CBPeripheral *peripheral = [object peripheralFor:address_string];
          found = [object characteristicOn:peripheral uuid:uuid_string] != nil;
      } @catch (NSException *exception) {
          (void)exception;
      }
    });
    if (!found) {
        return -ENOENT;
    }
    const int written = snprintf(out_handle, out_len, "%s/%s", address, char_uuid);
    return written > 0 && (size_t)written < out_len ? 0 : -ENAMETOOLONG;
}

int inkwell_ble_backend_mtu(struct inkwell_ble_central *central, const char *handle,
                            uint16_t *out_mtu) {
    IWCentral *object = object_of(central);
    NSString *handle_string = string_of(handle);
    __block NSUInteger payload = 0U;
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = nil;
      if ([object characteristicFor:handle_string peripheral:&peripheral] != nil) {
          /* The payload of one unacknowledged write is exactly `ATT MTU - 3`; the with-response
             figure is the long-write ceiling instead, which is not the MTU. */
          payload =
              [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
      }
    });
    if (payload == 0U) {
        return -ENOTSUP;
    }
    *out_mtu = (uint16_t)(payload + 3U);
    return 0;
}

int inkwell_ble_backend_subscribe(struct inkwell_ble_central *central, const char *handle) {
    IWCentral *object = object_of(central);
    NSString *handle_string = string_of(handle);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block bool started = false;
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = nil;
      CBCharacteristic *characteristic = [object characteristicFor:handle_string
                                                        peripheral:&peripheral];
      if (characteristic == nil) {
          return;
      }
      if (characteristic.isNotifying) {
          object.notifying = characteristic;
          object.subscribeResult = 0;
          dispatch_semaphore_signal(done);
          started = true;
          return;
      }
      object.subscribeDone = done;
      object.subscribeResult = -ETIMEDOUT;
      [peripheral setNotifyValue:YES forCharacteristic:characteristic];
      started = true;
    });
    if (!started) {
        return -ENOENT;
    }
    const long waited = dispatch_semaphore_wait(
        done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)CB_SUBSCRIBE_TIMEOUT_MS * NSEC_PER_MSEC));
    __block int result = -ETIMEDOUT;
    dispatch_sync(object.queue, ^{
      if (waited == 0) {
          result = object.subscribeResult;
      }
      object.subscribeDone = nil;
    });
    if (result < 0) {
        inkwell_log_warn("ble", "Subscribe failed on %s: %d", handle, result);
    }
    return result;
}

int inkwell_ble_backend_write(struct inkwell_ble_central *central, const char *handle,
                              const uint8_t *data, size_t len, uint32_t *token) {
    IWCentral *object = object_of(central);
    const uint32_t ours = next_token(backend_of(central));
    NSString *handle_string = string_of(handle);
    NSData *value = [NSData dataWithBytes:data length:len];
    __block int result = 0;
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = nil;
      CBCharacteristic *characteristic = [object characteristicFor:handle_string
                                                        peripheral:&peripheral];
      IWLink *link = peripheral != nil ? object.links[peripheral.identifier] : nil;
      if (characteristic == nil || link == nil) {
          result = peripheral != nil && peripheral.state != CBPeripheralStateConnected ? -ENOTCONN
                                                                                       : -ENOENT;
          return;
      }
      link.writeToken = ours;
      link.writeCharacteristic = characteristic;
      [peripheral writeValue:value
           forCharacteristic:characteristic
                        type:CBCharacteristicWriteWithResponse];
    });
    if (result == 0) {
        *token = ours;
    }
    return result;
}

int inkwell_ble_backend_read(struct inkwell_ble_central *central, const char *handle,
                             uint32_t *token) {
    IWCentral *object = object_of(central);
    const uint32_t ours = next_token(backend_of(central));
    NSString *handle_string = string_of(handle);
    __block int result = 0;
    dispatch_sync(object.queue, ^{
      CBPeripheral *peripheral = nil;
      CBCharacteristic *characteristic = [object characteristicFor:handle_string
                                                        peripheral:&peripheral];
      IWLink *link = peripheral != nil ? object.links[peripheral.identifier] : nil;
      if (characteristic == nil || link == nil) {
          result = peripheral != nil && peripheral.state != CBPeripheralStateConnected ? -ENOTCONN
                                                                                       : -ENOENT;
          return;
      }
      link.readToken = ours;
      link.readCharacteristic = characteristic;
      [peripheral readValueForCharacteristic:characteristic];
    });
    if (result == 0) {
        *token = ours;
    }
    return result;
}

static int wake_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    (void)inkwell_ble_process(userdata);
    return 0;
}

int inkwell_ble_backend_attach(struct inkwell_ble_central *central) {
    struct cb_backend *backend = backend_of(central);
    if (backend->wake_attached) {
        return 0;
    }
    const int result =
        inkwell_loop_add_fd(central->loop, backend->wake.fd, INKWELL_LOOP_IN, wake_ready, central);
    if (result == 0) {
        backend->wake_attached = true;
    }
    return result;
}

void inkwell_ble_backend_detach(struct inkwell_ble_central *central) {
    struct cb_backend *backend = backend_of(central);
    if (backend->wake_attached) {
        inkwell_loop_remove_fd(central->loop, backend->wake.fd);
        backend->wake_attached = false;
    }
}

static void apply(struct inkwell_ble_central *central, const struct cb_event *event) {
    switch (event->kind) {
    case CB_EVENT_CONNECT:
        if (central->connect_state == 1 && central->connect_token == event->token) {
            inkwell_ble_connect_finish(central, event->result);
        }
        break;
    case CB_EVENT_PAIR:
        if (central->pair_state == 1 && central->pair_token == event->token) {
            inkwell_ble_pair_finish(central, event->result);
        }
        break;
    case CB_EVENT_WRITE:
        if (central->requests[0].state == 1 && central->requests[0].token == event->token) {
            inkwell_ble_pending_finish(&central->requests[0], event->result);
        }
        break;
    case CB_EVENT_QUERY:
        if (event->which < 3U && central->requests[event->which].state == 1 &&
            central->requests[event->which].token == event->token) {
            central->requests[event->which].value = event->value;
            inkwell_ble_pending_finish(&central->requests[event->which], event->result);
        }
        break;
    case CB_EVENT_READ:
        if (central->read_state == 1 && central->read_token == event->token) {
            if (event->result == 0) {
                memcpy(central->read_payload, event->data, event->len);
                central->read_length = event->len;
            }
            inkwell_ble_read_finish(central, event->result);
        }
        break;
    case CB_EVENT_NOTIFY:
        inkwell_ble_notify(central, event->data, event->len);
        break;
    case CB_EVENT_LOG_INFO:
        inkwell_log_info("ble", "%s", (const char *)event->data);
        break;
    case CB_EVENT_LOG_WARN:
        inkwell_log_warn("ble", "%s", (const char *)event->data);
        break;
    }
}

int inkwell_ble_backend_process(struct inkwell_ble_central *central) {
    struct cb_backend *backend = backend_of(central);
    IWCentral *object = object_of(central);
    (void)inkwell_wake_drain(&backend->wake);
    __block NSArray<NSData *> *taken = nil;
    dispatch_sync(object.queue, ^{
      if (object.events.count > 0U) {
          taken = [object.events copy];
          [object.events removeAllObjects];
      }
    });
    for (NSData *bytes in taken) {
        struct cb_event event;
        memset(&event, 0, sizeof event);
        memcpy(&event, bytes.bytes, bytes.length < sizeof event ? bytes.length : sizeof event);
        apply(central, &event);
        /* A completion can close the central (a caller giving up on a link). */
        if (central->backend != backend) {
            break;
        }
    }
    return 0;
}
