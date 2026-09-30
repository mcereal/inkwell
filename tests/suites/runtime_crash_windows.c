#define WIN32_LEAN_AND_MEAN
#include "inkwell/runtime/crash.h"
#include <corecrt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char g_dir[INKWELL_CRASH_PATH_MAX];
static WCHAR g_dir_w[INKWELL_CRASH_PATH_MAX], g_exe[MAX_PATH];

static int install_report(void) {
    const struct inkwell_crash_config config = {
        .dir = g_dir,
        .product = "CrashTest",
        .log_warning = "Review the log before sharing.",
    };
    return inkwell_crash_install(&config);
}

__declspec(noinline) static void crash_access(void) {
    *(volatile int *)(uintptr_t)1U = 7;
}
__declspec(noinline) static void crash_access_caller(void) {
    crash_access();
}
__declspec(noinline) static void crash_stack(unsigned depth) {
    if (depth > 1000000U)
        return;
    volatile char space[4096];
    space[0] = (char)depth;
    crash_stack(depth + 1U);
    space[1] = space[0];
}

static int child(const char *mode) {
    if (install_report() != 0)
        return 20;
    if (strcmp(mode, "clean") == 0)
        return inkwell_crash_report_waiting() ? 1 : 0;
    if (strcmp(mode, "discard") == 0) {
        return !inkwell_crash_report_waiting() || inkwell_crash_discard() != 0 ||
                       inkwell_crash_report_waiting() || inkwell_crash_discard() != 0
                   ? 21
                   : 0;
    }
    if (strcmp(mode, "missing-dir") == 0) {
        WCHAR wide[INKWELL_CRASH_PATH_MAX];
        if (!MultiByteToWideChar(CP_UTF8, 0, g_dir, -1, wide, INKWELL_CRASH_PATH_MAX) ||
            !RemoveDirectoryW(wide))
            return 24;
        const bool discarded = inkwell_crash_discard() == 0 && inkwell_crash_discard() == 0 &&
                               !inkwell_crash_report_waiting();
        if (!CreateDirectoryW(wide, NULL))
            return 25;
        return discarded ? 0 : 26;
    }
    if (strcmp(mode, "access") == 0)
        crash_access_caller();
    if (strcmp(mode, "in-page") == 0) {
        const ULONG_PTR details[] = {0U, 0x12345U, 0xC000000EU};
        RaiseException(EXCEPTION_IN_PAGE_ERROR, 0U, 3U, details);
    }
    if (strcmp(mode, "refused") == 0) {
        static const char *const bad_labels[] = {"a-label-far-too-long-to-pad"};
        const struct inkwell_crash_config bad = {
            .dir = g_dir,
            .product = "Rejected",
            .log_warning = "Wrong warning",
            .note_labels = bad_labels,
            .note_count = 1U,
        };
        if (inkwell_crash_install(&bad) != -EINVAL)
            return 23;
        crash_access_caller();
    }
    if (strcmp(mode, "abort") == 0)
        abort();
    if (strcmp(mode, "invalid") == 0)
        _invalid_parameter_noinfo_noreturn();
    if (strcmp(mode, "stack") == 0)
        crash_stack(0U);
    return 22;
}

static DWORD run_child(const char *mode) {
    WCHAR command[2U * MAX_PATH], mode_w[32];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, mode_w, 32);
    if (swprintf(command, sizeof command / sizeof command[0], L"\"%ls\" child %ls \"%ls\"", g_exe,
                 mode_w, g_dir_w) < 0)
        return 0xFFFFFFFFU;
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(g_exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup,
                        &process))
        return 0xFFFFFFFFU;
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000U);
    DWORD result = 0xFFFFFFFFU;
    if (wait == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 99U);
    else
        GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

static bool read_report(char *body, size_t cap) {
    body[0] = '\0';
    WCHAR path[INKWELL_CRASH_PATH_MAX];
    if (swprintf(path, INKWELL_CRASH_PATH_MAX, L"%ls\\crash.txt", g_dir_w) < 0)
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD read = 0U;
    const bool ok = ReadFile(file, body, (DWORD)cap - 1U, &read, NULL);
    body[read] = '\0';
    CloseHandle(file);
    return ok;
}

static bool binary_has_codeview(void) {
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *pe = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY debug =
        pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debug.VirtualAddress == 0U || debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
        return false;
    const IMAGE_DEBUG_DIRECTORY *entries =
        (const IMAGE_DEBUG_DIRECTORY *)(base + debug.VirtualAddress);
    for (size_t i = 0U; i < debug.Size / sizeof(*entries); ++i) {
        if (entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW && entries[i].SizeOfData >= 24U &&
            memcmp(base + entries[i].AddressOfRawData, "RSDS", 4U) == 0)
            return true;
    }
    return false;
}

static int check(bool condition, const char *message) {
    if (!condition)
        fprintf(stderr, "[FAIL] %s\n", message);
    return !condition;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "child") == 0) {
        strncpy(g_dir, argv[3], sizeof g_dir - 1U);
        return child(argv[2]);
    }
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    WCHAR temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"iwc", 0U, g_dir_w) ||
        !DeleteFileW(g_dir_w) || !CreateDirectoryW(g_dir_w, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, g_dir_w, -1, g_dir, sizeof g_dir, NULL, NULL))
        return check(false, "could not make a test directory");
    int failures = 0;
    char body[16384];
    failures += check(run_child("clean") == 0U, "empty directory reported a waiting crash");
    failures += check(run_child("access") == EXCEPTION_ACCESS_VIOLATION, "access violation exit");
    failures += check(read_report(body, sizeof body), "access violation left no report");
    failures += check(strstr(body, "0xc0000005 (EXCEPTION_ACCESS_VIOLATION)") != NULL,
                      "exception code or name missing");
    failures += check(strstr(body, "fault addr   0x0000000000000001") != NULL,
                      "access violation address missing");
    failures +=
        check(strstr(body, "load base    0x") != NULL && strstr(body, "image size   0x") != NULL &&
                  strstr(body, "code id      ") != NULL,
              "PE image identity missing");
    if (binary_has_codeview())
        failures += check(strstr(body, "build id     RSDS ") != NULL,
                          "CodeView binary has no RSDS build id in its report");
    failures += check(strstr(body, " #01 ") != NULL, "faulting stack has fewer than two frames");
    failures +=
        check(run_child("in-page") == EXCEPTION_IN_PAGE_ERROR && read_report(body, sizeof body) &&
                  strstr(body, "EXCEPTION_IN_PAGE_ERROR") != NULL &&
                  strstr(body, "fault addr   0x0000000000012345") != NULL,
              "in-page error did not report the inaccessible address");
    failures += check(
        run_child("refused") == EXCEPTION_ACCESS_VIOLATION && read_report(body, sizeof body) &&
            strstr(body, "CrashTest crash report") != NULL && strstr(body, "Rejected") == NULL,
        "refused install changed the active report");
    failures += check(run_child("discard") == 0U, "waiting report was not discarded");
    failures += check(!read_report(body, sizeof body), "discard left the report on disk");
    failures += check(run_child("missing-dir") == 0U,
                      "discard failed after the report directory was removed");
    failures += check(run_child("abort") != 0U && read_report(body, sizeof body) &&
                          strstr(body, "SIGABRT") != NULL,
                      "abort left no report");
    failures += check(run_child("invalid") != 0U && read_report(body, sizeof body) &&
                          strstr(body, "CRT_INVALID_PARAMETER") != NULL,
                      "invalid CRT parameter left no report");
    failures += check(
        run_child("stack") == EXCEPTION_STACK_OVERFLOW && read_report(body, sizeof body) &&
            strstr(body, "EXCEPTION_STACK_OVERFLOW") != NULL && strstr(body, "--- end") != NULL,
        "stack overflow left no complete report");
    WCHAR path[INKWELL_CRASH_PATH_MAX];
    swprintf(path, INKWELL_CRASH_PATH_MAX, L"%ls\\crash.txt", g_dir_w);
    DeleteFileW(path);
    RemoveDirectoryW(g_dir_w);
    if (failures == 0)
        fprintf(stderr, "[PASS] Windows crash reports\n");
    return failures != 0;
}
