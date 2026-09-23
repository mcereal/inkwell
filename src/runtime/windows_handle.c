#include "windows_handle.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#define WINDOWS_HANDLE_MAX 64
#define WINDOWS_HANDLE_TOKEN_BASE 0x40000000

static HANDLE s_handles[WINDOWS_HANDLE_MAX];
static SRWLOCK s_handles_lock = SRWLOCK_INIT;

int inkwell_windows_handle_register(HANDLE handle) {
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return -EINVAL;
    }
    AcquireSRWLockExclusive(&s_handles_lock);
    for (size_t i = 0; i < WINDOWS_HANDLE_MAX; ++i) {
        if (s_handles[i] == NULL) {
            s_handles[i] = handle;
            ReleaseSRWLockExclusive(&s_handles_lock);
            return WINDOWS_HANDLE_TOKEN_BASE + (int)i;
        }
    }
    ReleaseSRWLockExclusive(&s_handles_lock);
    return -ENOSPC;
}

HANDLE inkwell_windows_handle_get(int token) {
    const int index = token - WINDOWS_HANDLE_TOKEN_BASE;
    if (index < 0 || index >= WINDOWS_HANDLE_MAX) {
        return NULL;
    }
    AcquireSRWLockShared(&s_handles_lock);
    HANDLE handle = s_handles[index];
    ReleaseSRWLockShared(&s_handles_lock);
    return handle;
}

void inkwell_windows_handle_close(int token) {
    const int index = token - WINDOWS_HANDLE_TOKEN_BASE;
    if (index < 0 || index >= WINDOWS_HANDLE_MAX) {
        return;
    }
    AcquireSRWLockExclusive(&s_handles_lock);
    HANDLE handle = s_handles[index];
    s_handles[index] = NULL;
    ReleaseSRWLockExclusive(&s_handles_lock);
    if (handle != NULL) {
        (void)CloseHandle(handle);
    }
}
