#pragma once

#include "inkwell/runtime/wake.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What a source waits for and what it is told, in the loop's own words.
 *
 * The values are epoll's, bit for bit, and loop.c asserts it on Linux: so there the mask goes
 * straight into epoll_ctl() and straight back out of epoll_wait() with nothing translated, and a
 * caller that still says EPOLLIN means the same thing. The kqueue backend on macOS maps these
 * onto read and write filters. Windows waitable handles report the mask they registered, while
 * Winsock notifications are translated to these bits when the event is dispatched.
 *
 * ERR and HUP are reported whether or not they were asked for, as epoll reports them. Asking for
 * them is harmless and says what the callback is prepared to hear.
 *
 * PRI is "something exceptional": a socket's out-of-band byte, and on Linux the way the kernel
 * signals through a file rather than with data - a pressure trigger (/proc/pressure/<resource>)
 * that has fired, a sysfs attribute that changed. Linux only: elsewhere a source may ask for it,
 * and still hears IN and OUT as asked, but is never told PRI.
 */
#define INKWELL_LOOP_IN 0x001U
#define INKWELL_LOOP_PRI 0x002U
#define INKWELL_LOOP_OUT 0x004U
#define INKWELL_LOOP_ERR 0x008U
#define INKWELL_LOOP_HUP 0x010U

typedef int (*inkwell_loop_callback)(int fd, uint32_t events, void *userdata);

/*
 * How many sources one loop holds, its own wake among them: a fixed array, so a loop never
 * allocates. 32 unless the build
 * says otherwise - the INKWELL_LOOP_MAX_SOURCES cache variable, which CMake puts on the inkwell
 * target as a PUBLIC definition, so the library and every program that links it agree on the
 * struct. A server that is one source per client (a display server, a bus) needs more than 32;
 * a microcontroller fewer. On Windows the wait is WaitForMultipleObjects(), so at most
 * MAXIMUM_WAIT_OBJECTS (64), which loop.c asserts.
 */
#ifndef INKWELL_LOOP_MAX_SOURCES
#define INKWELL_LOOP_MAX_SOURCES 32
#endif

struct inkwell_loop_source {
    int fd;
    uint32_t events;
    inkwell_loop_callback callback;
    void *userdata;
    bool active;
#if defined(_WIN32)
    uintptr_t socket;
    void *socket_event;
    bool is_socket;
#endif
};

struct inkwell_loop {
    int poll_fd; /* the epoll instance on Linux, the kqueue on macOS, unused on Windows */
    struct inkwell_wake wake;
    bool running;
    bool stop_requested;
    struct inkwell_loop_source sources[INKWELL_LOOP_MAX_SOURCES];
};

int inkwell_loop_init(struct inkwell_loop *loop);
void inkwell_loop_shutdown(struct inkwell_loop *loop);

int inkwell_loop_add_fd(struct inkwell_loop *loop, int fd, uint32_t events,
                        inkwell_loop_callback callback, void *userdata);
int inkwell_loop_update_fd(struct inkwell_loop *loop, int fd, uint32_t events);
int inkwell_loop_remove_fd(struct inkwell_loop *loop, int fd);

/* Watch a native socket on any host. Returns a nonnegative registration token, or negative errno.
 * The token is used with update_fd/remove_fd and passed to the callback; it is not necessarily
 * the socket itself. The caller owns the socket and must remove the watch before closing it. */
int inkwell_loop_watch_socket(struct inkwell_loop *loop, uintptr_t socket, uint32_t events,
                              inkwell_loop_callback callback, void *userdata);

#if defined(_WIN32)
/* Register a native Winsock SOCKET without narrowing its pointer-sized value to int. The
 * returned nonnegative token is passed to the callback and to update/remove_fd; the caller
 * retains ownership of the socket. WSAEventSelect makes the socket nonblocking. */
int inkwell_loop_add_socket(struct inkwell_loop *loop, uintptr_t socket, uint32_t events,
                            inkwell_loop_callback callback, void *userdata);
#endif

int inkwell_loop_run(struct inkwell_loop *loop, int timeout_ms);
void inkwell_loop_request_stop(struct inkwell_loop *loop);

#ifdef __cplusplus
}
#endif
