#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A descriptor that becomes readable when somebody says so.
 *
 * It is how a part of the program that is not a descriptor - a store that changed, a drain that
 * wants another turn, a request to stop - gets the loop's attention the same way a socket does.
 * On Linux that is one eventfd, and `fd` and `write_fd` are the same number. Elsewhere it is a
 * pipe, because nothing else is both pollable and portable, and the two ends differ.
 *
 * `fd` is the one to hand to inkwell_loop_add_fd(). Close it with inkwell_wake_close() rather
 * than close(), which would leak the other end of a pipe.
 *
 * Signalling is a count on Linux and a byte per signal elsewhere, and neither is an interface: a
 * wake says "look", not "look three times". inkwell_wake_drain() empties it whichever it is.
 */
struct inkwell_wake {
    int fd;
    int write_fd;
};

/* Opens a non-blocking, close-on-exec wake. 0 or a negative errno; on failure both are -1. */
int inkwell_wake_open(struct inkwell_wake *wake);

/* Makes `fd` readable. Safe to repeat: a wake that is already pending stays pending, and a full
   pipe is a pending wake rather than an error. 0 or a negative errno. */
int inkwell_wake_signal(const struct inkwell_wake *wake);

/* Empties it, so the next poll sees nothing until the next signal. Returns whether anything was
   pending - 1 or 0 - or a negative errno. */
int inkwell_wake_drain(const struct inkwell_wake *wake);

/* Closes both ends and sets them to -1. Safe on a wake that never opened. */
void inkwell_wake_close(struct inkwell_wake *wake);

#ifdef __cplusplus
}
#endif
