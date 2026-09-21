#define _POSIX_C_SOURCE 200809L

/*
 * The crash reporter.
 *
 * Split in two on purpose, because half of this module cannot be tested the way the rest of the
 * suite tests anything.
 *
 * The **writer** is ordinary code and is checked in-process through inkwell_crash_write_report():
 * the notes come out under their headings, the log ring keeps the newest lines in the order they
 * were logged, and the file says the things a reader has to be able to find in it.
 *
 * The **handler** cannot be. It is installed process-wide, it is reached only by a real fault,
 * and the fault it is reached by kills whatever raised it - so a case that raised SIGSEGV in the
 * test binary would take the runner down with it, and one that called the handler directly would
 * be checking a function rather than the thing that matters, which is whether a *crashing
 * process* leaves a file behind. So those cases fork: the child installs, faults for real, and
 * dies, and the parent reads the file the child left and the status the kernel reported.
 *
 * The forking is not only about surviving the fault. A signal disposition is per process and so
 * is everything else in that module, and this binary runs the whole suite in one process - the
 * app cases install a handler of their own long before these run, since mesh_app_init() does.
 * A child gets its own copy of those statics, so each case here starts from a state it controls
 * rather than from whatever ran first alphabetically. That ordering is exactly what caught the
 * install bug `crash_install_re_aims_rather_than_ignoring_a_second_call` now pins.
 */

#include "inkwell/base/log.h"

#include "framework/inkwell_test.h"
#include "support/fs_fixture.h"

#include "inkwell/runtime/crash.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Read a whole file into `out`. False when it is missing or bigger than the buffer, which for a
   report means something is wrong with it rather than with the reading. */
static bool crash_test_slurp(const char *path, char *out, size_t out_len) {
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    const size_t read = fread(out, 1U, out_len - 1U, file);
    out[read] = '\0';
    const bool complete = feof(file) != 0;
    (void)fclose(file);
    return complete;
}

/* A directory of this case's own under /tmp, in the shape the rest of the suite uses. */
static bool crash_test_tempdir(char *template_path) {
    return mkdtemp(template_path) != NULL;
}

/*
 * The identity a test application declares, and the one call every case here installs through.
 *
 * It stands in for what an application passes: a name, a binary, somewhere to send the file,
 * and - the field with no default - a sentence about what its own log may contain. The slots
 * below are the three a real application was found to want, and they are this suite's, not the
 * module's: inkwell_crash_note() takes an index and knows nothing about what is in it.
 */
enum { CRASH_NOTE_VERSION = 0, CRASH_NOTE_ROUTE, CRASH_NOTE_TRANSPORT, CRASH_NOTE_COUNT };

static const char *const k_crash_note_labels[CRASH_NOTE_COUNT] = {
    [CRASH_NOTE_VERSION] = "version",
    [CRASH_NOTE_ROUTE] = "route",
    [CRASH_NOTE_TRANSPORT] = "transport",
};

static int crash_test_install(const char *dir) {
    const struct inkwell_crash_config config = {
        .dir = dir,
        .product = "TestApp",
        .binary = "testapp",
        .issues_url = "https://example.invalid/issues",
        .log_warning = "Depending on what you were doing they can name people and places, or\n"
                       "quote a message you sent.",
        .note_labels = k_crash_note_labels,
        .note_count = CRASH_NOTE_COUNT,
    };
    const int result = inkwell_crash_install(&config);
    if (result == 0) {
        /*
         * The wiring an application does, and the reason it is here rather than inside the
         * install: log.h keeps the sink a decision nothing but the application makes, so this
         * helper makes it the way a real startup would.
         */
        inkwell_log_set_sink(inkwell_crash_log_line);
    }
    return result;
}

/* ---- the writer ------------------------------------------------------------------------------ */

INKWELL_TEST_CASE(crash_report_carries_its_notes, unit) {
    inkwell_crash_note(CRASH_NOTE_VERSION, "9.9.9-test");
    inkwell_crash_note(CRASH_NOTE_ROUTE, "nodes/map");
    inkwell_crash_note(CRASH_NOTE_TRANSPORT, "connected: Test Radio");

    char dir[] = "/tmp/inkwell_crash_notesXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");
    /* The notes above were set before this, deliberately: a label arriving later must not lose
       a fact recorded earlier, which is the ordering a caller should not have to think about. */
    INKWELL_TEST_FAIL_IF_CLEANUP(crash_test_install(dir) != 0, inkwell_test_remove_tree(dir),
                                 "the install refused a usable config");

    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    INKWELL_TEST_FAIL_IF_CLEANUP(file == NULL, inkwell_test_remove_tree(dir),
                                 "could not open a report");
    inkwell_crash_write_report(fileno(file), 11);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");

    INKWELL_TEST_FAIL_IF(strstr(body, "9.9.9-test") == NULL, "the version note is missing");
    INKWELL_TEST_FAIL_IF(strstr(body, "nodes/map") == NULL, "the route note is missing");
    INKWELL_TEST_FAIL_IF(strstr(body, "connected: Test Radio") == NULL,
                         "the transport note is missing");
    /* The signal is named as well as numbered: a bug report quoting "11" and one quoting
       "SIGSEGV" should not be two different conversations. */
    INKWELL_TEST_FAIL_IF(strstr(body, "SIGSEGV") == NULL, "the signal was not named");

    /*
     * The warning the file opens with is load-bearing rather than decorative: it is what a user
     * reads before deciding whether to attach the thing to a public issue.
     *
     * The assertion is that it *warns*, and deliberately not that it reassures. The first
     * version of this file claimed to hold "no message text, no node names, no coordinates" and
     * three quarters of that was false - the log tail below is the ordinary log, which quotes
     * outbound messages and names channels and places. A test pinning the reassuring wording
     * would have locked the wrong half in.
     */
    INKWELL_TEST_FAIL_IF(strstr(body, "Please read it before attaching it") == NULL,
                         "the report no longer asks to be read before it is shared");
    INKWELL_TEST_FAIL_IF(strstr(body, "quote a message you sent") == NULL,
                         "the report no longer warns what its log tail can contain");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_report_keeps_the_newest_log_lines, unit) {
    /*
     * Two full turns of the ring plus a bit, so nothing any other case logged can still be in
     * it - which is what makes the assertions below about *absence* mean anything in a binary
     * where every other suite is logging as it runs.
     */
    const unsigned pushed = INKWELL_CRASH_LOG_LINES * 2U + 3U;
    for (unsigned i = 0U; i < pushed; ++i) {
        char line[64];
        snprintf(line, sizeof line, "ring line %u", i);
        inkwell_crash_log_line(line);
    }

    char dir[] = "/tmp/inkwell_crash_ringXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(crash_test_install(dir) != 0, inkwell_test_remove_tree(dir),
                                 "the install refused a usable config");
    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    INKWELL_TEST_FAIL_IF_CLEANUP(file == NULL, inkwell_test_remove_tree(dir),
                                 "could not open a report");
    inkwell_crash_write_report(fileno(file), 6);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");

    /* The last line pushed is the last line in the file: what was happening immediately before
       the fault is the whole reason the ring exists. */
    char newest[64];
    snprintf(newest, sizeof newest, "ring line %u", pushed - 1U);
    INKWELL_TEST_FAIL_IF(strstr(body, newest) == NULL, "the newest line is not in the report");

    /* And the line that fell off the far end is gone rather than lingering. */
    char evicted[64];
    snprintf(evicted, sizeof evicted, "ring line %u\n", pushed - INKWELL_CRASH_LOG_LINES - 1U);
    INKWELL_TEST_FAIL_IF(strstr(body, evicted) != NULL, "an evicted line is still in the report");

    /*
     * Oldest first. A ring written newest-first reads as a stack trace run backwards, and the
     * one thing somebody does with this section is start at the bottom - so the order is part of
     * the format rather than an implementation detail.
     */
    char oldest[64];
    snprintf(oldest, sizeof oldest, "ring line %u\n", pushed - INKWELL_CRASH_LOG_LINES);
    const char *first = strstr(body, oldest);
    const char *last = strstr(body, newest);
    INKWELL_TEST_FAIL_IF(first == NULL, "the oldest surviving line is missing");
    INKWELL_TEST_FAIL_IF(last == NULL || first > last,
                         "the log is not in the order it was written");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_log_ring_takes_what_the_logger_formats, unit) {
    /*
     * The tap is in inkwell_log_message_v(), so a line reaches the ring already carrying its level
     * and its component. Checked through the logger rather than through inkwell_crash_log_line()
     * directly, because what would break here is the wiring rather than the ring.
     */
    const enum inkwell_log_level restore = inkwell_log_get_level();
    inkwell_log_set_sink(inkwell_crash_log_line);
    inkwell_log_set_level(INKWELL_LOG_LEVEL_INFO);
    inkwell_log_info("crashtest", "a marker worth %d cents", 42);

    char dir[] = "/tmp/inkwell_crash_tapXXXXXX";
    INKWELL_TEST_FAIL_IF_CLEANUP(!crash_test_tempdir(dir), inkwell_log_set_level(restore),
                                 "mkdtemp failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(crash_test_install(dir) != 0, inkwell_log_set_level(restore);
                                 inkwell_test_remove_tree(dir), "the install refused a config");
    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    INKWELL_TEST_FAIL_IF_CLEANUP(file == NULL, inkwell_log_set_level(restore);
                                 inkwell_test_remove_tree(dir), "could not open a report");
    inkwell_crash_write_report(fileno(file), 6);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    inkwell_log_set_level(restore);
    INKWELL_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");
    INKWELL_TEST_FAIL_IF(strstr(body, "a marker worth 42 cents") == NULL,
                         "the logger's line never reached the ring");
    INKWELL_TEST_FAIL_IF(strstr(body, "(crashtest)") == NULL,
                         "the component was dropped on the way");

    /*
     * Below the level in force, so the log file never showed it and the ring must not either.
     * The two have to agree: a report holding lines the log does not is a report that disagrees
     * with the file it is meant to be pasted beside.
     */
    inkwell_log_set_level(INKWELL_LOG_LEVEL_ERROR);
    inkwell_log_debug("crashtest", "this line is below the level");
    inkwell_log_set_level(restore);

    char second[256];
    snprintf(second, sizeof second, "%s", path);
    char dir2[] = "/tmp/inkwell_crash_tap2XXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir2), "mkdtemp failed");
    snprintf(second, sizeof second, "%s/report.txt", dir2);
    FILE *again = fopen(second, "we");
    INKWELL_TEST_FAIL_IF_CLEANUP(again == NULL, inkwell_test_remove_tree(dir2), "could not open");
    inkwell_crash_write_report(fileno(again), 6);
    (void)fclose(again);
    const bool read2 = crash_test_slurp(second, body, sizeof body);
    inkwell_test_remove_tree(dir2);
    INKWELL_TEST_FAIL_IF(!read2, "second report was unreadable");
    INKWELL_TEST_FAIL_IF(strstr(body, "below the level") != NULL,
                         "a line the log filtered out reached the ring");
    record_success(test_name);
}

/* ---- the handler, from a process that really crashes ------------------------------------------
 */

/*
 * Fault in a child and report how it went.
 *
 * `mode` picks what the child does once it has installed. The child never returns: it either
 * dies of the signal - which is the case under test - or _exit()s with a code the parent can
 * tell apart from a signal death.
 */
enum crash_child_mode {
    CRASH_CHILD_SEGV = 0,
    CRASH_CHILD_ABORT,
    CRASH_CHILD_CLEAN,
    CRASH_CHILD_STACK_OVERFLOW,
};

/*
 * Four calls deep, so a walk that stops early is visibly wrong rather than plausibly short. The
 * `volatile` argument is what stops the compiler folding the three together and handing the walk
 * one frame to find.
 *
 * The fault is `raise()` rather than a dereference of NULL, and that is not squeamishness. A
 * null dereference is undefined behaviour, which means a sanitizer is entitled to do something
 * other than let it fault - and UBSan does exactly that: by default it *reports* the load and
 * lets the program carry on, so under the sanitizer build these children never died at all and
 * every case here failed on a report that was never written. Asking for the signal directly is
 * what the handler's contract is actually about - a fatal signal arrives, a report is written,
 * and the process still dies of it - and it behaves the same under every build this repo makes.
 *
 * What the raise does not exercise is a genuinely bad `si_addr` on a genuinely damaged stack;
 * nothing here asserts on either, and a real dereference was used by hand to confirm the report
 * resolves through addr2line to the faulting line.
 */
static int crash_child_three(volatile int *p) {
    (void)p;
    raise(SIGSEGV);
    return 0;
}
static int crash_child_two(volatile int *p) {
    return crash_child_three(p) + 1;
}
static int crash_child_one(volatile int *p) {
    return crash_child_two(p) + 1;
}

/*
 * Run the stack out.
 *
 * Recursive, with an argument that depends on the recursion so nothing can turn it into a loop,
 * and a local array big enough to get there quickly. The `volatile` is what stops the whole
 * thing being optimised away as having no effect.
 */
static int crash_child_exhaust_stack(int depth) {
    volatile char block[4096];
    /* `| 1` so the byte is always odd and so never zero. Written as (char)depth first time out,
       which reached exactly depth 256, truncated to 0, took the guard below and returned - a
       recursion that terminates is a stack that never runs out, and the case passed nothing. */
    block[0] = (char)(depth | 1);
    block[sizeof block - 1U] = (char)(depth | 1);
    if (block[0] == 0) {
        return 0; /* unreachable, and deliberately not provably so */
    }
    return crash_child_exhaust_stack(depth + 1) + (int)block[sizeof block - 1U];
}

static pid_t crash_test_fork_child(const char *dir, enum crash_child_mode mode) {
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    if (crash_test_install(dir) != 0) {
        _exit(40);
    }
    inkwell_crash_note(CRASH_NOTE_VERSION, "child-build");
    inkwell_crash_note(CRASH_NOTE_ROUTE, "status/trend");
    inkwell_log_set_level(INKWELL_LOG_LEVEL_INFO);
    inkwell_log_info("child", "the last thing the child did");

    switch (mode) {
    case CRASH_CHILD_SEGV:
        /* Reached only if the signal did not kill us, which is itself a failure worth a code of
           its own rather than a silent pass. */
        _exit(crash_child_one((volatile int *)0) == 0 ? 41 : 42);
    case CRASH_CHILD_ABORT:
        abort();
    case CRASH_CHILD_STACK_OVERFLOW:
        _exit(crash_child_exhaust_stack(1) == 0 ? 50 : 51);
    case CRASH_CHILD_CLEAN:
    default:
        _exit(inkwell_crash_report_waiting() ? 1 : 0);
    }
}

INKWELL_TEST_CASE(crash_handler_writes_a_report_from_a_real_fault, unit) {
    char dir[] = "/tmp/inkwell_crash_faultXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");

    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");

    /*
     * The child died *of the signal*, rather than exiting tidily from inside the handler.
     *
     * This is the half that is easy to lose. A handler that swallowed the fault would leave a
     * process still running on a corrupted stack, and whatever started the client - the launcher
     * on a Brick, a shell here - would be told it exited normally. The re-raise at the end of
     * the handler is what keeps that honest, and this is the only place it is checked.
     */
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFSIGNALED(status), inkwell_test_remove_tree(dir),
                                 "the child did not die of its signal");
    INKWELL_TEST_FAIL_IF_CLEANUP(WTERMSIG(status) != SIGSEGV, inkwell_test_remove_tree(dir),
                                 "the child died of the wrong signal");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, INKWELL_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "no readable report was left behind");

    INKWELL_TEST_FAIL_IF(strstr(body, "SIGSEGV") == NULL, "the report does not name the signal");
    INKWELL_TEST_FAIL_IF(strstr(body, "child-build") == NULL, "the notes were lost");
    INKWELL_TEST_FAIL_IF(strstr(body, "the last thing the child did") == NULL,
                         "the log ring was lost");
    /* The file is finished rather than cut off half way, which is what says the handler ran to
       the end instead of faulting inside itself. */
    INKWELL_TEST_FAIL_IF(strstr(body, "--- end") == NULL, "the report stops before its end marker");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_handler_walks_more_than_one_frame, unit) {
    /*
     * Its own case rather than another assertion on the one above, because it is the part most
     * likely to quietly stop working: the walk is guarded so heavily that every way of getting
     * it wrong ends in *fewer frames*, never in a crash or an error. It shipped once requiring
     * sixteen-byte-aligned frame pointers, which is true on aarch64 and false on x86-64, and the
     * symptom was a report with a single plausible address in it - indistinguishable from a
     * genuinely shallow stack unless something counts.
     *
     * Four calls deep plus the handler's own frames, so three is a floor a working walk clears
     * easily and a broken one cannot reach.
     */
    char dir[] = "/tmp/inkwell_crash_walkXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, INKWELL_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "no readable report was left behind");

    const char *stack = strstr(body, "--- stack");
    INKWELL_TEST_FAIL_IF(stack == NULL, "the report has no stack section");
    unsigned frames = 0U;
    for (const char *at = strstr(stack, " #"); at != NULL; at = strstr(at + 2, " #")) {
        ++frames;
    }
    INKWELL_TEST_FAIL_IF(frames < 3U, "the stack walk stopped after one or two frames");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_handler_catches_an_abort, unit) {
    /* SIGABRT is the one in the set that is not a fault: an assert, or libc finding something it
       refuses to continue past. It is worth the entry because it is how most *deliberate*
       stops arrive, and a set that caught only the four faults would miss all of them. */
    char dir[] = "/tmp/inkwell_crash_abortXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_ABORT);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT,
                                 inkwell_test_remove_tree(dir), "the child did not die of SIGABRT");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, INKWELL_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "an abort left no report");
    INKWELL_TEST_FAIL_IF(strstr(body, "SIGABRT") == NULL, "the report does not name the signal");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_handler_survives_an_exhausted_stack, unit) {
    /*
     * The case the alternate signal stack exists for, and the one that silently produced nothing
     * before it did.
     *
     * When the fault *is* the stack running out, the kernel has nowhere to build the signal
     * frame: without sigaltstack() it cannot deliver the signal, so the process dies having
     * never entered the handler - and the crash with the most interesting backtrace in it is the
     * one that leaves no file. Raised by review on this PR, and reproduced by taking SA_ONSTACK
     * back out, where this is the only case that notices.
     */
    char dir[] = "/tmp/inkwell_crash_stackXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    const pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_STACK_OVERFLOW);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFSIGNALED(status), inkwell_test_remove_tree(dir),
                                 "running the stack out did not kill the child");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, INKWELL_CRASH_REPORT_NAME);
    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "an exhausted stack left no report");
    INKWELL_TEST_FAIL_IF(strstr(body, "--- end") == NULL,
                         "the report stops before its end marker, so the handler died writing it");
    record_success(test_name);
}

/* ---- what the client is told afterwards --------------------------------------------------------
 */

INKWELL_TEST_CASE(crash_report_waiting_is_read_once_at_install, unit) {
    /*
     * The rule the About section and the banner both rest on: a client learns at startup whether
     * the *previous* run crashed, and never changes its mind afterwards.
     *
     * Asked on demand instead, the flag would flip the moment this run wrote its own report -
     * so a client would start telling the user it had crashed while they were still using it,
     * and the banner would appear underneath a fault that had not finished happening.
     */
    char dir[] = "/tmp/inkwell_crash_waitXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    /* Nothing there yet, so a fresh process says no. */
    pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_CLEAN);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
                                 inkwell_test_remove_tree(dir),
                                 "a directory with no report reported one waiting");

    /* Now crash one, and a *later* process finds it. */
    pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");

    pid = crash_test_fork_child(dir, CRASH_CHILD_CLEAN);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status) || WEXITSTATUS(status) != 1,
                                 inkwell_test_remove_tree(dir),
                                 "the report left by a previous run was not noticed");

    inkwell_test_remove_tree(dir);
    record_success(test_name);
}

/*
 * Discarding, from a child for the reason every other install here runs in one - and checked
 * from the parent, which can still see the directory after the child is gone.
 *
 * The exit codes are the assertions: 0 only if the file was there, the discard reported success,
 * the flag went down, and a second discard was also fine. That last one matters because the
 * banner's resolution is a press, and a press the user makes twice must not turn into an error
 * the second time.
 */
static pid_t crash_test_fork_discard(const char *dir) {
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    if (crash_test_install(dir) != 0) {
        _exit(40);
    }
    if (!inkwell_crash_report_waiting()) {
        _exit(41);
    }
    if (inkwell_crash_discard() != 0) {
        _exit(42);
    }
    if (inkwell_crash_report_waiting()) {
        _exit(43);
    }
    /* Again, on nothing. */
    if (inkwell_crash_discard() != 0) {
        _exit(44);
    }
    /* The path is still answerable with no report on disk: it is where one *would* go, which is
       what lets the About row say so before anything has gone wrong. */
    char path[INKWELL_CRASH_PATH_MAX];
    if (!inkwell_crash_report_path(path, sizeof path) || path[0] == '\0') {
        _exit(45);
    }
    _exit(0);
}

INKWELL_TEST_CASE(crash_discard_removes_the_report_and_repeats_cleanly, unit) {
    char dir[] = "/tmp/inkwell_crash_discardXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");

    pid_t pid = crash_test_fork_child(dir, CRASH_CHILD_SEGV);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");

    pid = crash_test_fork_discard(dir);
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(dir), "fork failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(dir),
                                 "waitpid failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!WIFEXITED(status), inkwell_test_remove_tree(dir),
                                 "the discarding child died");

    const int code = WEXITSTATUS(status);
    INKWELL_TEST_FAIL_IF_CLEANUP(code == 41, inkwell_test_remove_tree(dir),
                                 "the report was not seen as waiting");
    INKWELL_TEST_FAIL_IF_CLEANUP(code == 42, inkwell_test_remove_tree(dir), "the discard failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(code == 43, inkwell_test_remove_tree(dir),
                                 "the flag stayed up after a discard");
    INKWELL_TEST_FAIL_IF_CLEANUP(code == 44, inkwell_test_remove_tree(dir),
                                 "a second discard reported a failure");
    INKWELL_TEST_FAIL_IF_CLEANUP(code == 45, inkwell_test_remove_tree(dir),
                                 "the path stopped being answerable once the report was gone");
    INKWELL_TEST_FAIL_IF_CLEANUP(code != 0, inkwell_test_remove_tree(dir),
                                 "the discarding child failed");

    /* And from out here: the file really is gone from the filesystem, not merely from the
       module's opinion of it. */
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, INKWELL_CRASH_REPORT_NAME);
    struct stat info;
    const bool still_there = stat(path, &info) == 0;
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(still_there, "the report is still on disk after a discard");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_install_re_aims_rather_than_ignoring_a_second_call, unit) {
    /*
     * A second install points the report at the new directory.
     *
     * This is here because the opposite - returning early once installed - is the obvious
     * implementation and hides a real bug: the signal disposition is genuinely once per process,
     * but the *path* is not, so a caller passing a different directory got success and the old
     * location. It surfaced as every forked case in this file inheriting an install from
     * whichever suite ran first and writing its report somewhere nobody was looking, which is
     * precisely the shape of failure a caller never thinks to check for.
     */
    char first[] = "/tmp/inkwell_crash_aim1XXXXXX";
    char second[] = "/tmp/inkwell_crash_aim2XXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(first), "mkdtemp failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(!crash_test_tempdir(second), inkwell_test_remove_tree(first),
                                 "mkdtemp failed");

    const pid_t pid = fork();
    INKWELL_TEST_FAIL_IF_CLEANUP(pid < 0, inkwell_test_remove_tree(first);
                                 inkwell_test_remove_tree(second), "fork failed");
    if (pid == 0) {
        if (crash_test_install(first) != 0) {
            _exit(40);
        }
        if (crash_test_install(second) != 0) {
            _exit(41);
        }
        char path[INKWELL_CRASH_PATH_MAX];
        if (!inkwell_crash_report_path(path, sizeof path)) {
            _exit(42);
        }
        _exit(strstr(path, second) != NULL ? 0 : 43);
    }
    int status = 0;
    INKWELL_TEST_FAIL_IF_CLEANUP(waitpid(pid, &status, 0) != pid, inkwell_test_remove_tree(first);
                                 inkwell_test_remove_tree(second), "waitpid failed");
    const bool re_aimed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    inkwell_test_remove_tree(first);
    inkwell_test_remove_tree(second);
    INKWELL_TEST_FAIL_IF(!re_aimed, "a second install did not re-aim the report");
    record_success(test_name);
}

INKWELL_TEST_CASE(crash_install_refuses_what_it_cannot_name, unit) {
    /* A directory whose name leaves no room for the report's own is refused rather than
       truncated: a path cut short is a path that names some other file. */
    char dir[INKWELL_CRASH_PATH_MAX + 64U];
    memset(dir, 'a', sizeof dir - 1U);
    dir[0] = '/';
    dir[sizeof dir - 1U] = '\0';

    const pid_t pid = fork();
    INKWELL_TEST_FAIL_IF(pid < 0, "fork failed");
    if (pid == 0) {
        _exit(crash_test_install(dir) == 0 ? 1 : 0);
    }
    int status = 0;
    INKWELL_TEST_FAIL_IF(waitpid(pid, &status, 0) != pid, "waitpid failed");
    INKWELL_TEST_FAIL_IF(!WIFEXITED(status) || WEXITSTATUS(status) != 0,
                         "an over-long directory was accepted");

    /* An empty one is the other end of the same question. */
    INKWELL_TEST_FAIL_IF(crash_test_install("") == 0, "an empty directory was accepted");
    INKWELL_TEST_FAIL_IF(crash_test_install(NULL) == 0, "a NULL directory was accepted");
    INKWELL_TEST_FAIL_IF(inkwell_crash_install(NULL) == 0, "a NULL config was accepted");

    /*
     * The two fields with no sensible default. A product name is what every heading in the file
     * is built out of, and the log warning is the only sentence in it this layer cannot write -
     * see the top of inkwell/runtime/crash.h for why a default one would be a lie.
     */
    const struct inkwell_crash_config nameless = {.dir = "/tmp", .log_warning = "anything"};
    INKWELL_TEST_FAIL_IF(inkwell_crash_install(&nameless) == 0, "a nameless product was accepted");
    const struct inkwell_crash_config unwarned = {.dir = "/tmp", .product = "TestApp"};
    INKWELL_TEST_FAIL_IF(inkwell_crash_install(&unwarned) == 0, "a missing warning was accepted");

    /* And a label too long to pad, which would line a value up under the wrong heading. */
    static const char *const k_long[] = {"a-label-far-too-long-to-pad"};
    const struct inkwell_crash_config wide = {.dir = "/tmp",
                                              .product = "TestApp",
                                              .log_warning = "anything",
                                              .note_labels = k_long,
                                              .note_count = 1U};
    INKWELL_TEST_FAIL_IF(inkwell_crash_install(&wide) == 0, "an unpaddable label was accepted");
    record_success(test_name);
}

/*
 * A refused install leaves the last accepted one exactly as it was.
 *
 * This is the case for a bug that was real: the config was applied field by field and validated
 * as it went, so an install rejected on a late field had already replaced the title, the intro,
 * the addr2line line and - the one that matters - the privacy warning, and had cleared every
 * note label on the way. The handler installed by the *previous*, accepted call was still live,
 * so a crash a moment later wrote a report naming a product the caller had been told was
 * rejected, warning about a log it had been told was rejected, and carrying none of the notes
 * the process had been keeping.
 *
 * Checked through the report rather than through the return value, because the return value was
 * always right. -EINVAL was reported correctly the whole time; what was wrong was everything it
 * had already done.
 */
INKWELL_TEST_CASE(crash_a_refused_install_changes_nothing, unit) {
    char dir[] = "/tmp/inkwell_crash_refusedXXXXXX";
    INKWELL_TEST_FAIL_IF(!crash_test_tempdir(dir), "mkdtemp failed");
    INKWELL_TEST_FAIL_IF_CLEANUP(crash_test_install(dir) != 0, inkwell_test_remove_tree(dir),
                                 "the accepted install was refused");
    inkwell_crash_note(CRASH_NOTE_VERSION, "9.9.9-accepted");
    inkwell_crash_note(CRASH_NOTE_ROUTE, "a-route-that-must-survive");

    char kept[INKWELL_CRASH_PATH_MAX];
    INKWELL_TEST_FAIL_IF_CLEANUP(!inkwell_crash_report_path(kept, sizeof kept),
                                 inkwell_test_remove_tree(dir), "no path after a good install");

    /* Three configs, each refused for a different reason, and each carrying an identity that
       must not reach the report: a label too long to pad, a NULL label, and a product name
       longer than the heading can lay out. */
    static const char *const k_long_label[] = {"version", "a-label-far-too-long-to-pad"};
    static const char *const k_null_label[] = {"version", NULL};
    static char k_long_product[INKWELL_CRASH_PRODUCT_MAX + 8U];
    memset(k_long_product, 'X', sizeof k_long_product - 1U);
    k_long_product[sizeof k_long_product - 1U] = '\0';

    const struct inkwell_crash_config k_refused[] = {
        {.dir = dir,
         .product = "RejectedApp",
         .log_warning = "REJECTED WARNING",
         .note_labels = k_long_label,
         .note_count = 2U},
        {.dir = dir,
         .product = "RejectedApp",
         .log_warning = "REJECTED WARNING",
         .note_labels = k_null_label,
         .note_count = 2U},
        {.dir = dir, .product = k_long_product, .log_warning = "REJECTED WARNING"},
    };
    for (size_t i = 0; i < sizeof k_refused / sizeof k_refused[0]; ++i) {
        INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_crash_install(&k_refused[i]) != -EINVAL,
                                     inkwell_test_remove_tree(dir), "a bad config was accepted");
    }

    /* And a directory with no room for the report's name, which fails later than the rest. */
    static char k_long_dir[INKWELL_CRASH_PATH_MAX + 16U];
    memset(k_long_dir, 'd', sizeof k_long_dir - 1U);
    k_long_dir[0] = '/';
    k_long_dir[sizeof k_long_dir - 1U] = '\0';
    const struct inkwell_crash_config deep = {
        .dir = k_long_dir, .product = "RejectedApp", .log_warning = "REJECTED WARNING"};
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_crash_install(&deep) != -ENAMETOOLONG,
                                 inkwell_test_remove_tree(dir), "an unusable directory was taken");

    /* The path is the one the accepted install set, not cleared and not re-aimed. */
    char now[INKWELL_CRASH_PATH_MAX];
    INKWELL_TEST_FAIL_IF_CLEANUP(
        !inkwell_crash_report_path(now, sizeof now) || strcmp(now, kept) != 0,
        inkwell_test_remove_tree(dir), "a refused install moved or cleared the report path");

    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    INKWELL_TEST_FAIL_IF_CLEANUP(file == NULL, inkwell_test_remove_tree(dir), "could not open");
    inkwell_crash_write_report(fileno(file), 11);
    (void)fclose(file);

    static char body[16384];
    const bool read = crash_test_slurp(path, body, sizeof body);
    inkwell_test_remove_tree(dir);
    INKWELL_TEST_FAIL_IF(!read, "report was unreadable or longer than the buffer");

    INKWELL_TEST_FAIL_IF(strstr(body, "RejectedApp") != NULL || strstr(body, "XXXX") != NULL,
                         "a refused config's product name reached the report");
    INKWELL_TEST_FAIL_IF(strstr(body, "REJECTED WARNING") != NULL,
                         "a refused config's privacy warning reached the report");
    INKWELL_TEST_FAIL_IF(strstr(body, "TestApp crash report") == NULL,
                         "the accepted config's title did not survive");
    INKWELL_TEST_FAIL_IF(strstr(body, "9.9.9-accepted") == NULL ||
                             strstr(body, "a-route-that-must-survive") == NULL,
                         "a refused install dropped the notes the process was keeping");
    record_success(test_name);
}
