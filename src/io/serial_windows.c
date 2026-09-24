#include "inkwell/io/serial.h"

#include "../runtime/windows_handle.h"
#include "inkwell/base/fd.h"

/* windows_handle.h has brought in winsock2.h and windows.h in the order Winsock needs. */
// clang-format off
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>
// clang-format on

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The serial ports Windows has, and one opened for the loop.
 *
 * The scan asks SetupAPI for every present COM port interface and keeps the ones on USB: the
 * instance ID carries the vendor and product ("USB\VID_303A&PID_1001&MI_00\..."), the device's
 * registry key its "COM4", and the driver's service name what the USB tree would have said on
 * Linux - usbser is CDC-ACM, which is the MCU's own USB; anything else is a bridge's driver.
 *
 * A COM port has no non-blocking descriptor, so an open one is three things behind one int:
 *
 * - the port, opened for overlapped I/O with timeouts that make a read return at once with
 *   whatever the driver holds, which is what a non-blocking read is;
 * - one manual-reset event the loop waits on, registered as a handle token. A WaitCommEvent for
 *   EV_RXCHAR and every write complete into it, so it is signalled when bytes arrive and when the
 *   port can take more, and the token is the "fd" the caller gets;
 * - the device ops that inkwell_fd_read/_write/_close route that token to.
 *
 * The event is reset by the read, which is the call a signalled event always gets - a caller
 * registered for IN is told IN on every signal. A read that stops with bytes still queued sets it
 * again, and so does one that collects a finished write while a later write is waiting on it, so
 * nothing that lands while the event is being reset is lost. A finished write nobody is waiting
 * on is collected quietly: re-signalling it would leave an idle port ready forever.
 */

#define SERIAL_WINDOWS_PORTS 8
#define SERIAL_WINDOWS_WRITE_BYTES 4096U

struct serial_windows_port {
    bool open;
    int token;
    HANDLE com;
    HANDLE read_event;
    OVERLAPPED wait_overlapped;
    DWORD wait_mask;
    bool wait_pending;
    OVERLAPPED write_overlapped;
    DWORD write_len;
    bool write_pending;
    /* A write answered -EAGAIN and its caller is waiting for the event to try again. */
    bool write_blocked;
    /* A write that finished short or failed after its caller had been told it was accepted; the
       next write reports it. */
    bool write_failed;
    /* A write is in flight from here, not from the caller's buffer, which is gone as soon as the
       call returns. */
    uint8_t write_bytes[SERIAL_WINDOWS_WRITE_BYTES];
};

static struct serial_windows_port s_ports[SERIAL_WINDOWS_PORTS];

struct inkwell_serial_windows_mock {
    bool enabled;
    struct inkwell_serial_mock_config config;
    size_t bind_calls;
    size_t line_state_calls;
    unsigned bind_pending_left;
};

static struct inkwell_serial_windows_mock g_mock;

/* ------------------------------------------------------------------ the scan */

/* GUID_DEVINTERFACE_COMPORT, from ntddser.h, spelled out so this file needs no second INITGUID
   header for one constant. */
static const GUID kComPortInterface = {
    0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

/* "VID_303A" or "VID_0403+" out of an instance ID - FTDI's bus driver writes its IDs with a
   plus where USB writes an ampersand. */
static bool instance_hex(const char *instance, const char *key, uint16_t *out) {
    const char *at = strstr(instance, key);
    if (at == NULL) {
        return false;
    }
    unsigned value = 0U;
    if (sscanf(at + strlen(key), "%4x", &value) != 1) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool devnode_service(DEVINST node, char *out, ULONG out_len) {
    ULONG len = out_len;
    const CONFIGRET result =
        CM_Get_DevNode_Registry_PropertyA(node, CM_DRP_SERVICE, NULL, out, &len, 0);
    if (result != CR_SUCCESS) {
        out[0] = '\0';
        return false;
    }
    out[out_len - 1U] = '\0';
    return true;
}

/* A mass-storage interface on the same composite device - what a UF2 bootloader presents beside
   its CDC pair. Only asked of an interface ("&MI_"): the parent of a whole-device function is a
   hub, and its children are other devices. */
static bool beside_mass_storage(DEVINST node) {
    DEVINST parent = 0;
    DEVINST child = 0;
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS ||
        CM_Get_Child(&child, parent, 0) != CR_SUCCESS) {
        return false;
    }
    do {
        char service[64];
        if (child != node && devnode_service(child, service, sizeof service) &&
            _stricmp(service, "USBSTOR") == 0) {
            return true;
        }
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
    return false;
}

/* The product string the device itself reported, which is what sysfs and the I/O Registry say
   too. Windows' own description ("USB Serial Device") names the driver, not the device. */
static bool bus_reported_name(HDEVINFO set, SP_DEVINFO_DATA *info, char *out, size_t out_len) {
    WCHAR name[128];
    DEVPROPTYPE type = 0;
    if (!SetupDiGetDevicePropertyW(set, info, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                                   (PBYTE)name, sizeof name, NULL, 0) ||
        type != DEVPROP_TYPE_STRING) {
        return false;
    }
    name[(sizeof name / sizeof name[0]) - 1U] = L'\0';
    return WideCharToMultiByte(CP_UTF8, 0, name, -1, out, (int)out_len, NULL, NULL) > 1;
}

static bool describe_port(HDEVINFO set, SP_DEVINFO_DATA *info,
                          struct inkwell_serial_port_info *port) {
    memset(port, 0, sizeof *port);
    char instance[200];
    if (CM_Get_Device_IDA(info->DevInst, instance, sizeof instance, 0) != CR_SUCCESS) {
        return false;
    }
    /* No vendor ID is a port that is not on USB - a motherboard's COM1 - which the header
       promises this scan does not report. */
    if (!instance_hex(instance, "VID_", &port->vendor_id) ||
        !instance_hex(instance, "PID_", &port->product_id)) {
        return false;
    }

    HKEY key = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD type = 0;
    DWORD len = sizeof port->path - 1U;
    const LSTATUS read = RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)port->path, &len);
    (void)RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_SZ || _strnicmp(port->path, "COM", 3) != 0) {
        return false;
    }

    /* Truncated when longer, which a USB instance ID never is in practice. */
    (void)snprintf(port->id, sizeof port->id, "%.63s", instance);
    if (!bus_reported_name(set, info, port->name, sizeof port->name)) {
        (void)snprintf(port->name, sizeof port->name, "USB serial %04x:%04x", port->vendor_id,
                       port->product_id);
    }
    char service[64];
    const bool cdc =
        devnode_service(info->DevInst, service, sizeof service) && _stricmp(service, "usbser") == 0;
    port->kind = cdc ? INKWELL_SERIAL_NATIVE : INKWELL_SERIAL_BRIDGE;
    port->mass_storage =
        cdc && strstr(instance, "&MI_") != NULL && beside_mass_storage(info->DevInst);
    port->control_interface = -1;
    port->bound = true;
    return true;
}

static size_t mock_scan(struct inkwell_serial_port_info *out, size_t capacity) {
    if (g_mock.config.scan_result < 0 || g_mock.config.ports == NULL) {
        return 0U;
    }
    const size_t count = g_mock.config.port_count < capacity ? g_mock.config.port_count : capacity;
    memcpy(out, g_mock.config.ports, count * sizeof *out);
    return count;
}

size_t inkwell_serial_scan(struct inkwell_serial_port_info *out, size_t capacity) {
    if (out == NULL || capacity == 0U) {
        return 0U;
    }
    if (g_mock.enabled) {
        return mock_scan(out, capacity);
    }
    HDEVINFO set =
        SetupDiGetClassDevsW(&kComPortInterface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return 0U;
    }
    size_t count = 0U;
    SP_DEVINFO_DATA info = {.cbSize = sizeof info};
    for (DWORD i = 0; count < capacity && SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        if (describe_port(set, &info, &out[count])) {
            count += 1U;
        }
    }
    (void)SetupDiDestroyDeviceInfoList(set);
    return count;
}

/* There is no driver to bind on Windows: a port the scan reports already has its COM name. */
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
    return port->bound && port->path[0] != '\0' ? 0 : -ENOTSUP;
}

/* usbser drives DTR itself, so the line state never has to go around the driver here. */
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

void inkwell_serial_set_sysfs_root(const char *root) {
    (void)root;
}

/* ------------------------------------------------------------------ an open port */

static struct serial_windows_port *port_for_token(int token) {
    for (size_t i = 0; i < SERIAL_WINDOWS_PORTS; ++i) {
        if (s_ports[i].open && s_ports[i].token == token) {
            return &s_ports[i];
        }
    }
    return NULL;
}

static int errno_from_win32(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return -ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        /* A COM port is exclusive: somebody else - a terminal, a flasher - has it open. */
        return -EBUSY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return -ENOMEM;
    default:
        return -EIO;
    }
}

/* Asks to be woken by the next byte. Returns 0, or a negative errno when the port has gone. */
static int port_arm_wait(struct serial_windows_port *port) {
    if (port->wait_pending) {
        return 0;
    }
    memset(&port->wait_overlapped, 0, sizeof port->wait_overlapped);
    port->wait_overlapped.hEvent = inkwell_windows_handle_get(port->token);
    port->wait_mask = 0;
    if (WaitCommEvent(port->com, &port->wait_mask, &port->wait_overlapped)) {
        /* It happened already; make sure the loop hears it. */
        (void)SetEvent(port->wait_overlapped.hEvent);
        return 0;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        return errno_from_win32(error);
    }
    port->wait_pending = true;
    return 0;
}

/* Takes a finished wait off the books so the next one can be armed. */
static void port_collect_wait(struct serial_windows_port *port) {
    if (!port->wait_pending || !HasOverlappedIoCompleted(&port->wait_overlapped)) {
        return;
    }
    DWORD ignored = 0;
    (void)GetOverlappedResult(port->com, &port->wait_overlapped, &ignored, FALSE);
    port->wait_pending = false;
}

/* Takes a finished write off the books, remembering a failure for the next write to report.
   Returns whether there is no write in flight any more. */
static bool port_collect_write(struct serial_windows_port *port) {
    if (!port->write_pending) {
        return true;
    }
    if (!HasOverlappedIoCompleted(&port->write_overlapped)) {
        return false;
    }
    DWORD written = 0;
    const BOOL finished = GetOverlappedResult(port->com, &port->write_overlapped, &written, FALSE);
    port->write_pending = false;
    if (!finished || written < port->write_len) {
        port->write_failed = true;
    }
    return true;
}

static int port_read(void *context, void *bytes, size_t len) {
    struct serial_windows_port *port = (struct serial_windows_port *)context;
    HANDLE event = inkwell_windows_handle_get(port->token);
    (void)ResetEvent(event);
    port_collect_wait(port);
    if (port_collect_write(port) && port->write_blocked) {
        /* The flush waiting on that write has not run yet; keep its wake-up. */
        (void)SetEvent(event);
    }

    DWORD errors = 0;
    COMSTAT status;
    if (!ClearCommError(port->com, &errors, &status)) {
        return 0; /* unplugged: the stream's EOF */
    }
    if (status.cbInQue == 0U) {
        if (port_arm_wait(port) < 0) {
            return 0;
        }
        /* A byte that landed between the count and the wait raised no event of its own. */
        if (ClearCommError(port->com, &errors, &status) && status.cbInQue > 0U) {
            (void)SetEvent(event);
        }
        return -EAGAIN;
    }

    const DWORD want = (DWORD)(len < status.cbInQue ? len : status.cbInQue);
    OVERLAPPED overlapped = {.hEvent = port->read_event};
    DWORD got = 0;
    if (!ReadFile(port->com, bytes, want, &got, &overlapped)) {
        const DWORD error = GetLastError();
        /* The bytes are already in the driver and the timeouts say not to wait for more, so a
           pending read completes at once. */
        if (error != ERROR_IO_PENDING || !GetOverlappedResult(port->com, &overlapped, &got, TRUE)) {
            return 0;
        }
    }
    if (got < status.cbInQue) {
        (void)SetEvent(event); /* more is queued than this call took */
    } else if (port_arm_wait(port) < 0) {
        return 0;
    }
    return got > 0U ? (int)got : -EAGAIN;
}

static int port_write(void *context, const void *bytes, size_t len) {
    struct serial_windows_port *port = (struct serial_windows_port *)context;
    if (port->write_pending && !port_collect_write(port)) {
        port->write_blocked = true;
        return -EAGAIN;
    }
    port->write_blocked = false;
    if (port->write_failed) {
        port->write_failed = false;
        return -EIO;
    }
    if (len == 0U) {
        return 0;
    }

    const DWORD count =
        (DWORD)(len < SERIAL_WINDOWS_WRITE_BYTES ? len : SERIAL_WINDOWS_WRITE_BYTES);
    memcpy(port->write_bytes, bytes, count);
    memset(&port->write_overlapped, 0, sizeof port->write_overlapped);
    port->write_overlapped.hEvent = inkwell_windows_handle_get(port->token);
    if (!WriteFile(port->com, port->write_bytes, count, NULL, &port->write_overlapped)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return errno_from_win32(error);
        }
    }
    /* Finished or not, it is collected by the next write, which is what reports a failure. */
    port->write_pending = true;
    port->write_len = count;
    return (int)count;
}

static int port_close(void *context) {
    struct serial_windows_port *port = (struct serial_windows_port *)context;
    /* RTS down before DTR, for the reason port_configure() raises them the other way round.
       Left to CloseHandle(), usbser drops them in its own order, and an ESP32-S3 on its own USB
       takes the moment between as a reset - measured on a Heltec V4, which rebooted on every
       close until this was here. */
    (void)EscapeCommFunction(port->com, CLRRTS);
    (void)EscapeCommFunction(port->com, CLRDTR);
    (void)CancelIoEx(port->com, NULL);
    DWORD ignored = 0;
    if (port->wait_pending) {
        (void)GetOverlappedResult(port->com, &port->wait_overlapped, &ignored, TRUE);
    }
    if (port->write_pending) {
        (void)GetOverlappedResult(port->com, &port->write_overlapped, &ignored, TRUE);
    }
    (void)CloseHandle(port->com);
    (void)CloseHandle(port->read_event);
    inkwell_windows_handle_close(port->token);
    memset(port, 0, sizeof *port);
    return 0;
}

static const struct inkwell_fd_device_ops kPortOps = {
    .read = port_read,
    .write = port_write,
    .close = port_close,
};

/*
 * 8N1, no flow control, and DTR and RTS both raised - what a POSIX open leaves a tty at, and
 * what firmware waiting for a host takes as one being there.
 *
 * DTR goes first. An ESP32 treats RTS raised while DTR is low as a reset, whether through a
 * bridge board's auto-program transistors or its own USB Serial/JTAG, so the lines go
 * (0,0) -> (1,0) -> (1,1) here and back the same way on close, and never pass through (0,1).
 */
static bool port_configure(HANDLE com, unsigned baud) {
    DCB dcb = {.DCBlength = sizeof dcb};
    if (!GetCommState(com, &dcb)) {
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(com, &dcb)) {
        return false;
    }
    /* A read returns at once with whatever is queued, even nothing; a write takes as long as it
       takes, on its own overlapped time. */
    COMMTIMEOUTS timeouts = {.ReadIntervalTimeout = MAXDWORD};
    return EscapeCommFunction(com, SETRTS) && SetCommTimeouts(com, &timeouts) &&
           SetCommMask(com, EV_RXCHAR) && PurgeComm(com, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

int inkwell_serial_open(const char *path, unsigned baud) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    if (g_mock.enabled) {
        if (g_mock.config.open_result < 0) {
            return g_mock.config.open_result;
        }
        return g_mock.config.open_fd >= 0 ? inkwell_fd_dup(g_mock.config.open_fd) : -ENOENT;
    }
    if (baud == 0U) {
        return -EINVAL;
    }
    struct serial_windows_port *port = NULL;
    for (size_t i = 0; i < SERIAL_WINDOWS_PORTS && port == NULL; ++i) {
        port = s_ports[i].open ? NULL : &s_ports[i];
    }
    if (port == NULL) {
        return -EMFILE;
    }

    /* "\\.\COM10" and above only open by their device path; COM1..9 open either way. */
    char device[80];
    const bool qualified = strncmp(path, "\\\\.\\", 4) == 0;
    if (snprintf(device, sizeof device, "%s%s", qualified ? "" : "\\\\.\\", path) >=
        (int)sizeof device) {
        return -ENAMETOOLONG;
    }
    HANDLE com = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, NULL);
    if (com == INVALID_HANDLE_VALUE) {
        return errno_from_win32(GetLastError());
    }
    if (!port_configure(com, baud)) {
        const DWORD error = GetLastError();
        (void)CloseHandle(com);
        return error == ERROR_INVALID_PARAMETER ? -EINVAL : errno_from_win32(error);
    }

    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    const int token = ready != NULL ? inkwell_windows_handle_register(ready) : -ENOMEM;
    if (token < 0 || read_event == NULL) {
        if (token >= 0) {
            inkwell_windows_handle_close(token);
        } else if (ready != NULL) {
            (void)CloseHandle(ready);
        }
        if (read_event != NULL) {
            (void)CloseHandle(read_event);
        }
        (void)CloseHandle(com);
        return token < 0 ? token : -ENOMEM;
    }

    memset(port, 0, sizeof *port);
    port->open = true;
    port->token = token;
    port->com = com;
    port->read_event = read_event;
    const int attached = inkwell_fd_attach_device(token, &kPortOps, port);
    if (attached < 0) {
        (void)port_close(port);
        return attached;
    }
    if (port_arm_wait(port) < 0) {
        (void)inkwell_fd_close(token);
        return -EIO;
    }
    return token;
}

void inkwell_serial_close(int fd) {
    if (fd >= 0) {
        (void)inkwell_fd_close(fd);
    }
}

int inkwell_serial_set_dtr(int fd, bool on) {
    if (fd < 0) {
        return -EINVAL;
    }
    struct serial_windows_port *port = port_for_token(fd);
    if (port == NULL) {
        return -ENOTTY;
    }
    if (!EscapeCommFunction(port->com, on ? SETDTR : CLRDTR)) {
        return errno_from_win32(GetLastError());
    }
    return 0;
}

/* ------------------------------------------------------------------ the mock */

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
