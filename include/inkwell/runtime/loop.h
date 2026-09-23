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
 * onto read and write filters. Windows waitable handles currently report the mask they registered;
 * Winsock events join this abstraction in the networking slice.
 *
 * ERR and HUP are reported whether or not they were asked for, as epoll reports them. Asking for
 * them is harmless and says what the callback is prepared to hear.
 */
#define INKWELL_LOOP_IN 0x001U
#define INKWELL_LOOP_OUT 0x004U
#define INKWELL_LOOP_ERR 0x008U
#define INKWELL_LOOP_HUP 0x010U

typedef int (*inkwell_loop_callback)(int fd, uint32_t events, void *userdata);

#define INKWELL_LOOP_MAX_SOURCES 32

struct inkwell_loop_source {
    int fd;
    uint32_t events;
    inkwell_loop_callback callback;
    void *userdata;
    bool active;
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

int inkwell_loop_run(struct inkwell_loop *loop, int timeout_ms);
void inkwell_loop_request_stop(struct inkwell_loop *loop);

#ifdef __cplusplus
}
#endif
