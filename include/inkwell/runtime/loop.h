#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    int epoll_fd;
    int wake_fd;
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
