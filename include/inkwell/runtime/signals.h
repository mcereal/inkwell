#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* Shutdown delivered through a source on the event loop - a signalfd on Linux, a kqueue on macOS,
   and a console control event on Windows - so it runs the normal path instead of the default kill
   action. Nothing here runs in POSIX signal or Windows console-handler context. */
struct inkwell_signals {
    struct inkwell_loop *loop;
    int fd;
};

int inkwell_signals_init(struct inkwell_signals *signals, struct inkwell_loop *loop);
void inkwell_signals_shutdown(struct inkwell_signals *signals);

#ifdef __cplusplus
}
#endif
