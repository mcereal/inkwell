/* Windows fault capture; crash_report.c owns the report's text and section order. */
#include "crash_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <errno.h>
#include <io.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The reserved fd writes through Win32 without allocating a CRT descriptor in the handler. */
#define CRASH_HANDLE_FD (-2)
static HANDLE g_handle = INVALID_HANDLE_VALUE;
static WCHAR g_path[INKWELL_CRASH_PATH_MAX];
static char g_load_base[32], g_image_size[32], g_build_id[80], g_code_id[32];
static bool g_installed;
static volatile LONG g_reporting;

static bool crash_widen(const char *path, WCHAR *out) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out,
                               INKWELL_CRASH_PATH_MAX) > 0;
}

ptrdiff_t inkwell_crash_internal_backend_write(int fd, const char *data, size_t len) {
    if (fd != CRASH_HANDLE_FD) {
        return (ptrdiff_t)_write(fd, data, (unsigned)len);
    }
    DWORD written = 0;
    const DWORD count = len > UINT32_MAX ? UINT32_MAX : (DWORD)len;
    return WriteFile(g_handle, data, count, &written, NULL) ? (ptrdiff_t)written : 0;
}

bool inkwell_crash_internal_path_exists(const char *path) {
    WCHAR wide[INKWELL_CRASH_PATH_MAX];
    return crash_widen(path, wide) && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

int inkwell_crash_internal_unlink(const char *path) {
    WCHAR wide[INKWELL_CRASH_PATH_MAX];
    if (!crash_widen(path, wide)) {
        errno = EINVAL;
        return -1;
    }
    if (DeleteFileW(wide)) {
        return 0;
    }
    const DWORD error = GetLastError();
    errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EIO;
    return -1;
}

int inkwell_crash_internal_discard_without_path(void) {
    return -EINVAL;
}

static void crash_hex(char *out, size_t cap, uint64_t value, size_t digits) {
    (void)inkwell_crash_internal_format_unsigned(out, cap, value, 16U, digits);
}

static void crash_capture_image(void) {
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    if (base == NULL) {
        return;
    }
    g_load_base[0] = '0';
    g_load_base[1] = 'x';
    crash_hex(g_load_base + 2, sizeof g_load_base - 2U, (uint64_t)(uintptr_t)base, 16U);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return;
    }
    const IMAGE_NT_HEADERS64 *pe = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }
    g_image_size[0] = '0';
    g_image_size[1] = 'x';
    crash_hex(g_image_size + 2, sizeof g_image_size - 2U, pe->OptionalHeader.SizeOfImage, 0U);
    crash_hex(g_code_id, sizeof g_code_id, pe->FileHeader.TimeDateStamp, 8U);
    const size_t at = strlen(g_code_id);
    crash_hex(g_code_id + at, sizeof g_code_id - at, pe->OptionalHeader.SizeOfImage, 0U);

    const IMAGE_DATA_DIRECTORY debug =
        pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debug.VirtualAddress == 0U || debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        debug.VirtualAddress >= pe->OptionalHeader.SizeOfImage ||
        debug.Size > pe->OptionalHeader.SizeOfImage - debug.VirtualAddress) {
        return;
    }
    const IMAGE_DEBUG_DIRECTORY *entries =
        (const IMAGE_DEBUG_DIRECTORY *)(base + debug.VirtualAddress);
    for (size_t i = 0U; i < debug.Size / sizeof(*entries); ++i) {
        const IMAGE_DEBUG_DIRECTORY *entry = &entries[i];
        if (entry->Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry->SizeOfData < 24U ||
            entry->AddressOfRawData >= pe->OptionalHeader.SizeOfImage ||
            entry->SizeOfData > pe->OptionalHeader.SizeOfImage - entry->AddressOfRawData) {
            continue;
        }
        const uint8_t *rsds = base + entry->AddressOfRawData;
        if (memcmp(rsds, "RSDS", 4U) != 0) {
            continue;
        }
        const GUID *guid = (const GUID *)(rsds + 4U);
        uint32_t age;
        memcpy(&age, rsds + 20U, sizeof age);
        char *out = g_build_id;
        memcpy(out, "RSDS ", 5U);
        out += 5U;
        crash_hex(out, 9U, guid->Data1, 8U);
        out += 8U;
        crash_hex(out, 5U, guid->Data2, 4U);
        out += 4U;
        crash_hex(out, 5U, guid->Data3, 4U);
        out += 4U;
        for (unsigned j = 0U; j < 8U; ++j) {
            crash_hex(out, 3U, guid->Data4[j], 2U);
            out += 2U;
        }
        crash_hex(out, sizeof g_build_id - (size_t)(out - g_build_id), age, 0U);
        return;
    }
}

static const char *crash_exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    default:
        return "EXCEPTION_UNKNOWN";
    }
}

static void crash_write_frames(int fd, uint64_t context_address) {
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT context = *(const CONTEXT *)(uintptr_t)context_address;
    for (unsigned depth = 0U; depth < 32U && context.Rip != 0U; ++depth) {
        inkwell_crash_internal_puts(fd, " #");
        char index[8];
        const size_t len =
            inkwell_crash_internal_format_unsigned(index, sizeof index, depth, 10U, 2U);
        inkwell_crash_internal_write(fd, index, len);
        inkwell_crash_internal_puts(fd, " ");
        inkwell_crash_internal_write_address(fd, context.Rip);
        inkwell_crash_internal_puts(fd, "\n");
        DWORD64 image_base = 0U;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (function != NULL) {
            PVOID handler_data = NULL;
            DWORD64 frame = 0U;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &frame, NULL);
        } else {
            DWORD64 return_address = 0U;
            SIZE_T read = 0U;
            if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)context.Rsp,
                                   &return_address, sizeof return_address, &read) ||
                read != sizeof return_address) {
                break;
            }
            context.Rsp += sizeof return_address;
            context.Rip = return_address;
        }
    }
#else
    (void)fd;
    (void)context_address;
#endif
}

static void crash_report(DWORD code, const char *name, EXCEPTION_POINTERS *exception) {
    if (InterlockedCompareExchange(&g_reporting, 1, 0) != 0 || g_path[0] == L'\0') {
        return;
    }
    g_handle = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    const EXCEPTION_RECORD *record = exception != NULL ? exception->ExceptionRecord : NULL;
    const CONTEXT *context = exception != NULL ? exception->ContextRecord : NULL;
    CrashFault fault = {
        .signal_number = code,
        .signal_hex = exception != NULL,
        .signal_name = name,
        .has_info = record != NULL,
        .code = record != NULL ? (int64_t)record->ExceptionCode : 0,
        .fault_address =
            record != NULL &&
                    (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
                    record->NumberParameters >= 2U
                ? record->ExceptionInformation[1]
            : record != NULL ? (uint64_t)(uintptr_t)record->ExceptionAddress
                             : 0U,
        .load_base = g_load_base,
        .build_id = g_build_id,
        .code_id = g_code_id,
        .image_size = g_image_size,
    };
#if defined(_M_X64) || defined(__x86_64__)
    if (context != NULL) {
        fault.have_registers = true;
        fault.pc = context->Rip;
        fault.fp = (uint64_t)(uintptr_t)context;
    }
#endif
    inkwell_crash_internal_write_report(CRASH_HANDLE_FD, &fault, crash_write_frames);
    (void)FlushFileBuffers(g_handle);
    (void)CloseHandle(g_handle);
    g_handle = INVALID_HANDLE_VALUE;
}

static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *exception) {
    crash_report(exception->ExceptionRecord->ExceptionCode,
                 crash_exception_name(exception->ExceptionRecord->ExceptionCode), exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_abort(int signal_number) {
    crash_report((DWORD)signal_number, "SIGABRT", NULL);
    (void)signal(SIGABRT, SIG_DFL);
    (void)raise(SIGABRT);
    TerminateProcess(GetCurrentProcess(), (UINT)signal_number);
}

static void crash_invalid_parameter(const wchar_t *expression, const wchar_t *function,
                                    const wchar_t *file, unsigned line, uintptr_t reserved) {
    (void)expression;
    (void)function;
    (void)file;
    (void)line;
    (void)reserved;
    crash_report(0xE0000001U, "CRT_INVALID_PARAMETER", NULL);
    TerminateProcess(GetCurrentProcess(), 0xE0000001U);
}

static void crash_purecall(void) {
    crash_report(0xE0000002U, "CRT_PURECALL", NULL);
    TerminateProcess(GetCurrentProcess(), 0xE0000002U);
}

int inkwell_crash_install(const struct inkwell_crash_config *config) {
    if (config == NULL || config->dir == NULL) {
        return -EINVAL;
    }
    char path[INKWELL_CRASH_PATH_MAX];
    const size_t dir_length = strlen(config->dir);
    const size_t name_length = sizeof INKWELL_CRASH_REPORT_NAME;
    if (dir_length + name_length >= sizeof path) {
        return -ENAMETOOLONG;
    }
    memcpy(path, config->dir, dir_length);
    path[dir_length] = '/';
    memcpy(path + dir_length + 1U, INKWELL_CRASH_REPORT_NAME, name_length);
    WCHAR wide[INKWELL_CRASH_PATH_MAX];
    if (!crash_widen(path, wide)) {
        return -EINVAL;
    }
    /* The filter needs committed stack space when the fault itself exhausted the thread stack. */
    ULONG guarantee = 64U * 1024U;
    if (!SetThreadStackGuarantee(&guarantee)) {
        return -EIO;
    }
    const int prepared = inkwell_crash_internal_prepare(config);
    if (prepared != 0) {
        return prepared;
    }
    memcpy(g_path, wide, ((size_t)lstrlenW(wide) + 1U) * sizeof(WCHAR));
    if (!g_installed) {
        crash_capture_image();
        SetUnhandledExceptionFilter(crash_exception_filter);
        if (signal(SIGABRT, crash_abort) == SIG_ERR) {
            return -EIO;
        }
        _set_invalid_parameter_handler(crash_invalid_parameter);
        _set_purecall_handler(crash_purecall);
        g_installed = true;
    }
    return 0;
}

void inkwell_crash_write_report(int fd, int signal_number) {
    const CrashFault fault = {
        .signal_number = (uint64_t)signal_number,
        .signal_name = signal_number == SIGABRT ? "SIGABRT" : "signal",
        .load_base = g_load_base,
        .build_id = g_build_id,
        .code_id = g_code_id,
        .image_size = g_image_size,
    };
    inkwell_crash_internal_write_report(fd, &fault, NULL);
}
