#include "inkwell/runtime/signals.h"

#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/wake.h"
#include "windows_handle.h"

#include <errno.h>
#include <windows.h>

static HANDLE s_console_event;

static BOOL WINAPI console_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT &&
        type != CTRL_LOGOFF_EVENT && type != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    return s_console_event != NULL && SetEvent(s_console_event) != 0;
}

static int signals_callback(int fd, uint32_t events, void *userdata) {
    struct inkwell_signals *signals = (struct inkwell_signals *)userdata;
    if (signals == NULL || (events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }
    struct inkwell_wake wake = {.fd = fd, .write_fd = fd};
    (void)inkwell_wake_drain(&wake);
    inkwell_loop_request_stop(signals->loop);
    return 0;
}

int inkwell_signals_init(struct inkwell_signals *signals, struct inkwell_loop *loop) {
    if (signals == NULL || loop == NULL) {
        return -EINVAL;
    }
    signals->loop = loop;
    signals->fd = -1;

    struct inkwell_wake wake;
    const int opened = inkwell_wake_open(&wake);
    if (opened < 0) {
        return opened;
    }
    signals->fd = wake.fd;
    s_console_event = inkwell_windows_handle_get(signals->fd);
    if (SetConsoleCtrlHandler(console_handler, TRUE) == 0) {
        s_console_event = NULL;
        inkwell_wake_close(&wake);
        signals->fd = -1;
        return -EIO;
    }
    const int added =
        inkwell_loop_add_fd(loop, signals->fd, INKWELL_LOOP_IN, signals_callback, signals);
    if (added < 0) {
        (void)SetConsoleCtrlHandler(console_handler, FALSE);
        s_console_event = NULL;
        inkwell_wake_close(&wake);
        signals->fd = -1;
        return added;
    }
    return 0;
}

void inkwell_signals_shutdown(struct inkwell_signals *signals) {
    if (signals == NULL) {
        return;
    }
    if (s_console_event != NULL) {
        (void)SetConsoleCtrlHandler(console_handler, FALSE);
        s_console_event = NULL;
    }
    if (signals->fd >= 0) {
        if (signals->loop != NULL) {
            (void)inkwell_loop_remove_fd(signals->loop, signals->fd);
        }
        inkwell_windows_handle_close(signals->fd);
        signals->fd = -1;
    }
    signals->loop = NULL;
}
