#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;
struct inkwell_child;

/*
 * A child process's exit, delivered as a source on the loop.
 *
 * One watch is one child, named by its pid, and nothing else. On Linux that is a pidfd: it
 * polls readable once the process has exited, it stays the same process however the pid is
 * reused, and it is what inkwell_child_signal() signals through. On macOS it is a kqueue
 * holding one EVFILT_PROC NOTE_EXIT, which the loop's kqueue waits on exactly as it waits on a
 * timer's. Windows has no pid to fork and refuses with -ENOSYS.
 *
 * Per child rather than SIGCHLD and waitpid(-1), because a reap-everything handler takes
 * children that are not its own: the forked resolver in net/resolve.c waits for its child by
 * pid, and a process-wide reaper would turn that into ECHILD. Here each watch reaps the one pid
 * it was given and leaves every other child where it was. A program that must also collect
 * children it never started - a process that is PID 1 inherits every orphan - does that itself,
 * and must skip the pids it has a watch on (waitid() with WNOWAIT looks without reaping).
 *
 * The watch reaps: when the child exits, its status is collected with waitpid(), the source is
 * removed and closed, and only then is `exited` called. So the callback may watch the same
 * struct again - a supervisor restarting a service - and the pid is gone from the process table
 * by the time it is told.
 *
 * `status` is waitpid()'s, for WIFEXITED(), WEXITSTATUS(), WIFSIGNALED() and WTERMSIG(), or a
 * negative errno when the child is gone but its status could not be collected: -ECHILD when
 * something else reaped it first, which is also what a SIGCHLD set to SIG_IGN does to every
 * child.
 *
 * A zeroed struct is an idle watch, so a struct that has never watched anything may be
 * cancelled.
 */
typedef void (*inkwell_child_exited)(struct inkwell_child *child, int status, void *userdata);

struct inkwell_child {
    struct inkwell_loop *loop; /* NULL while idle */
    int fd;                    /* the pidfd on Linux, the kqueue on macOS */
    int pid;                   /* the child watched; kept once idle, for the callback to read */
    inkwell_child_exited exited;
    void *userdata;
};

/*
 * Watches `pid`, which must be a child of this process that has not been reaped; one that has
 * already exited is reported on the next turn of the loop. 0, or a negative errno with the watch
 * left idle: -ECHILD when `pid` is not an unreaped child, -EBUSY when `child` is already
 * watching, -ENOSYS where the system cannot (Windows, or a Linux kernel older than 5.3).
 */
int inkwell_child_watch(struct inkwell_child *child, struct inkwell_loop *loop, int pid,
                        inkwell_child_exited exited, void *userdata);

/* Sends `signal` to the watched child: through the pidfd on Linux, so it can never reach a
   process that inherited a recycled pid. 0, or a negative errno; -ESRCH when nothing is being
   watched. */
int inkwell_child_signal(const struct inkwell_child *child, int signal);

/* Stops watching without reaping and without calling `exited`. The child is the caller's again,
   to wait for or to leave. Safe on an idle watch. */
void inkwell_child_cancel(struct inkwell_child *child);

#ifdef __cplusplus
}
#endif
