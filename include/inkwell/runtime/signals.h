#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* SIGINT/SIGTERM/SIGHUP delivered through a signalfd on the event loop, so a shutdown runs
   the normal path - whatever an application flushes on the way out - instead of the default
   kill action. Nothing here runs in signal context. */
struct inkwell_signals {
    struct inkwell_loop *loop;
    int fd;
};

int inkwell_signals_init(struct inkwell_signals *signals, struct inkwell_loop *loop);
void inkwell_signals_shutdown(struct inkwell_signals *signals);

#ifdef __cplusplus
}
#endif
