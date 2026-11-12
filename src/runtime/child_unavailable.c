#include "inkwell/runtime/child.h"

#include <errno.h>
#include <stddef.h>

/* Windows has no fork() to make a child with a pid to watch. A process HANDLE is the native
 * shape, and it will arrive with whatever starts one; until then every call refuses. */
int inkwell_child_watch(struct inkwell_child *child, struct inkwell_loop *loop, int pid,
                        inkwell_child_exited exited, void *userdata) {
    (void)loop;
    (void)pid;
    (void)exited;
    (void)userdata;
    return child == NULL ? -EINVAL : -ENOSYS;
}

int inkwell_child_signal(const struct inkwell_child *child, int signal) {
    (void)signal;
    return child == NULL ? -EINVAL : -ENOSYS;
}

void inkwell_child_cancel(struct inkwell_child *child) {
    (void)child;
}
