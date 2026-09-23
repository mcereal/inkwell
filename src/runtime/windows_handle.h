#pragma once

#if !defined(_WIN32)
#error "windows_handle.h is Windows-only"
#endif

/* winsock2.h must precede windows.h, which otherwise includes the older winsock.h. */
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

int inkwell_windows_handle_register(HANDLE handle);
HANDLE inkwell_windows_handle_get(int token);
void inkwell_windows_handle_close(int token);
