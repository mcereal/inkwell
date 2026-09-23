#pragma once

#if !defined(_WIN32)
#error "windows_handle.h is Windows-only"
#endif

#include <windows.h>

int inkwell_windows_handle_register(HANDLE handle);
HANDLE inkwell_windows_handle_get(int token);
void inkwell_windows_handle_close(int token);
