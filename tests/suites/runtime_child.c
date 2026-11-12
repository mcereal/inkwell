#define _POSIX_C_SOURCE 200809L

/*
 * A child's exit as a loop source: a pidfd on Linux, a kqueue's EVFILT_PROC on macOS. Each case
 * forks a real child, so what is checked is the kernel's behaviour on whichever of the two this
 * was built for.
 */

#include "framework/inkwell_test.h"

#include "inkwell/runtime/child.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct seen {
    int calls;
    int status;
    int pid;
};

static void on_exited(struct inkwell_child *child, int status, void *userdata) {
    struct seen *seen = (struct seen *)userdata;
    seen->calls++;
    seen->status = status;
    seen->pid = child->pid;
}

static pid_t spawn_exiting(int code) {
    const pid_t pid = fork();
    if (pid == 0) {
        _exit(code);
    }
    return pid;
}

static pid_t spawn_waiting(void) {
    const pid_t pid = fork();
    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    return pid;
}

static void sleep_ms(long ms) {
    struct timespec pause_for = {ms / 1000L, (ms % 1000L) * 1000000L};
    while (nanosleep(&pause_for, &pause_for) != 0) {
    }
}

/* Turns the loop until `calls` reaches `want`, or two seconds pass. */
static void run_until(struct inkwell_loop *loop, const int *calls, int want) {
    for (int i = 0; i < 200 && *calls < want; ++i) {
        (void)inkwell_loop_run(loop, 10);
    }
}

static bool reaped(pid_t pid) {
    return waitpid(pid, NULL, WNOHANG) < 0 && errno == ECHILD;
}

INKWELL_TEST_CASE(child_reports_the_exit_status, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    const pid_t pid = spawn_exiting(7);
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int watched = inkwell_child_watch(&child, &loop, (int)pid, on_exited, &seen);
    run_until(&loop, &seen.calls, 1);
    const bool gone = reaped(pid);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(watched != 0, "watch failed");
    INKWELL_TEST_FAIL_IF(seen.calls != 1, "the exit was not reported exactly once");
    INKWELL_TEST_FAIL_IF(seen.status < 0 || !WIFEXITED(seen.status), "not reported as an exit");
    INKWELL_TEST_FAIL_IF(WEXITSTATUS(seen.status) != 7, "the wrong exit code");
    INKWELL_TEST_FAIL_IF(seen.pid != (int)pid, "the callback could not tell which child it was");
    INKWELL_TEST_FAIL_IF(!gone, "the child was left unreaped");
    INKWELL_TEST_FAIL_IF(child.loop != NULL || child.fd != -1, "the watch was not released");
    record_success(test_name);
}

/* The callback is told after the release, so it may watch the same struct again - a supervisor
   restarting what just exited. */
static struct inkwell_loop *s_rewatch_loop;
static pid_t s_second;

static void on_exited_rewatch(struct inkwell_child *child, int status, void *userdata) {
    struct seen *seen = (struct seen *)userdata;
    seen->calls++;
    seen->status = status;
    if (seen->calls == 1) {
        s_second = spawn_exiting(2);
        if (inkwell_child_watch(child, s_rewatch_loop, (int)s_second, on_exited_rewatch, seen) !=
            0) {
            seen->calls = 100;
        }
    }
}

INKWELL_TEST_CASE(child_may_be_watched_again_from_its_callback, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    s_rewatch_loop = &loop;
    const pid_t first = spawn_exiting(1);
    INKWELL_TEST_FAIL_IF(first < 0, "fork failed");
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int watched = inkwell_child_watch(&child, &loop, (int)first, on_exited_rewatch, &seen);
    run_until(&loop, &seen.calls, 2);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(watched != 0, "watch failed");
    INKWELL_TEST_FAIL_IF(seen.calls != 2, "the restarted child was not reported");
    INKWELL_TEST_FAIL_IF(!WIFEXITED(seen.status) || WEXITSTATUS(seen.status) != 2,
                         "the second exit was not the second child's");
    INKWELL_TEST_FAIL_IF(!reaped(first) || !reaped(s_second), "a child was left unreaped");
    record_success(test_name);
}

/* A child that is already a zombie when it is watched: Darwin will not attach NOTE_EXIT to one,
   and the watch must still report it. */
INKWELL_TEST_CASE(child_that_already_exited_is_reported, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    const pid_t pid = spawn_exiting(3);
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    sleep_ms(50);
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int watched = inkwell_child_watch(&child, &loop, (int)pid, on_exited, &seen);
    run_until(&loop, &seen.calls, 1);
    inkwell_loop_shutdown(&loop);
    if (watched != 0) {
        (void)waitpid(pid, NULL, 0);
    }
    INKWELL_TEST_FAIL_IF(watched != 0, "watching a zombie failed");
    INKWELL_TEST_FAIL_IF(seen.calls != 1, "a zombie's exit was not reported");
    INKWELL_TEST_FAIL_IF(!WIFEXITED(seen.status) || WEXITSTATUS(seen.status) != 3,
                         "the wrong exit code");
    record_success(test_name);
}

INKWELL_TEST_CASE(child_signal_reaches_the_child, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    const pid_t pid = spawn_waiting();
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int watched = inkwell_child_watch(&child, &loop, (int)pid, on_exited, &seen);
    const int signalled = inkwell_child_signal(&child, SIGTERM);
    run_until(&loop, &seen.calls, 1);
    const int after = inkwell_child_signal(&child, SIGTERM);
    inkwell_loop_shutdown(&loop);
    if (seen.calls == 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
    }
    INKWELL_TEST_FAIL_IF(watched != 0, "watch failed");
    INKWELL_TEST_FAIL_IF(signalled != 0, "signalling failed");
    INKWELL_TEST_FAIL_IF(seen.calls != 1, "the exit was not reported");
    INKWELL_TEST_FAIL_IF(!WIFSIGNALED(seen.status) || WTERMSIG(seen.status) != SIGTERM,
                         "not reported as killed by SIGTERM");
    INKWELL_TEST_FAIL_IF(after != -ESRCH, "signalling a finished watch did not refuse");
    record_success(test_name);
}

INKWELL_TEST_CASE(child_cancel_hands_the_child_back, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    const pid_t pid = spawn_exiting(0);
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int watched = inkwell_child_watch(&child, &loop, (int)pid, on_exited, &seen);
    inkwell_child_cancel(&child);
    sleep_ms(50);
    (void)inkwell_loop_run(&loop, 20);
    int status = 0;
    const pid_t got = waitpid(pid, &status, 0);
    inkwell_child_cancel(&child); /* twice is harmless */
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(watched != 0, "watch failed");
    INKWELL_TEST_FAIL_IF(seen.calls != 0, "a cancelled watch still called back");
    INKWELL_TEST_FAIL_IF(got != pid, "the cancelled watch reaped the child anyway");
    INKWELL_TEST_FAIL_IF(child.loop != NULL || child.fd != -1, "the watch was not released");
    record_success(test_name);
}

INKWELL_TEST_CASE(child_refuses_what_is_not_an_unreaped_child, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    struct seen seen = {0};

    struct inkwell_child self = {0};
    const int own = inkwell_child_watch(&self, &loop, (int)getpid(), on_exited, &seen);

    const pid_t pid = spawn_exiting(0);
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    (void)waitpid(pid, NULL, 0);
    struct inkwell_child gone = {0};
    const int reaped_already = inkwell_child_watch(&gone, &loop, (int)pid, on_exited, &seen);

    struct inkwell_child idle = {0};
    const int signal_idle = inkwell_child_signal(&idle, SIGTERM);
    inkwell_child_cancel(&idle);

    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(own != -ECHILD, "watching this process itself did not refuse");
    INKWELL_TEST_FAIL_IF(reaped_already != -ECHILD, "watching a reaped pid did not refuse");
    INKWELL_TEST_FAIL_IF(self.loop != NULL || gone.loop != NULL, "a refused watch was kept");
    INKWELL_TEST_FAIL_IF(signal_idle != -ESRCH, "signalling an idle watch did not refuse");
    record_success(test_name);
}

INKWELL_TEST_CASE(child_refuses_a_second_watch_on_one_struct, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    const pid_t a = spawn_waiting();
    const pid_t b = spawn_exiting(0);
    INKWELL_TEST_FAIL_IF(a < 0 || b < 0, "fork failed");
    struct inkwell_child child = {0};
    struct seen seen = {0};
    const int first = inkwell_child_watch(&child, &loop, (int)a, on_exited, &seen);
    const int second = inkwell_child_watch(&child, &loop, (int)b, on_exited, &seen);
    const int still = child.pid;
    inkwell_child_cancel(&child);
    inkwell_loop_shutdown(&loop);
    (void)kill(a, SIGKILL);
    (void)waitpid(a, NULL, 0);
    (void)waitpid(b, NULL, 0);
    INKWELL_TEST_FAIL_IF(first != 0, "watch failed");
    INKWELL_TEST_FAIL_IF(second != -EBUSY, "a busy watch was overwritten");
    INKWELL_TEST_FAIL_IF(still != (int)a, "the first watch lost its pid");
    record_success(test_name);
}
