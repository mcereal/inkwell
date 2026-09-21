#pragma once

/*
 * What a process leaves behind when it faults.
 *
 * Until something like this exists a SIGSEGV takes a process down mid-sentence: a log piped
 * through `tee` simply stops, with nothing in it saying the process had died or where. A report
 * from a stranger is therefore "it crashed", and there is nothing to ask them for - the one fact
 * worth having is the one the fault destroyed.
 *
 * So this writes a file. Not a service, and deliberately nothing that leaves the device: the
 * memory of a faulted process holds whatever the application was holding, so a dump of it is the
 * last thing that should go anywhere by itself. What lands on disk is a page of text the user
 * can read before they decide to share it.
 *
 * **The file does not promise to be free of private data, and must not start.** That is why
 * `log_warning` below is required rather than optional. An earlier version of this wrote its own
 * assurance - "no message text, no names, no coordinates" - and three quarters of it was false,
 * because the log tail it carries is the application's ordinary log and an ordinary log names
 * things. A user deciding whether to attach the file to a public issue can act on "it may quote
 * a message you sent" and cannot act on an assurance that is wrong. This layer cannot know what
 * an application's log says, so it does not guess: the application states it and this prints it.
 * Redacting the ring instead would mean the logger knowing which of its arguments are private -
 * a real feature, and a larger one than this.
 *
 * ---- the discipline -------------------------------------------------------------------------
 *
 * A handler runs on a process that is already broken, which rules out most of libc. POSIX names
 * the functions that stay safe there and neither `printf` nor `malloc` is among them - a fault
 * inside `malloc` leaves the allocator's lock held, and a handler that takes it deadlocks
 * instead of reporting anything. So the writer here uses `write()` and formats its own integers,
 * and **every string it might need is built at install time**, in ordinary context, where
 * `snprintf` is allowed. That includes everything the caller passes in: the strings in
 * `struct inkwell_crash_config` are copied, and the headings they appear in are assembled, while
 * `inkwell_crash_install()` is running. A caller may free its own strings the moment that
 * returns, and the handler assembles nothing it did not already have.
 *
 * The one thing it must do that cannot be prepared is read the crashed stack, and that is why
 * `inkwell_crash_install()` makes a pipe it never sends anything through. Probing an address by
 * writing it to a descriptor turns an unreadable page into `EFAULT` - a return value - where
 * dereferencing it would be a second fault inside the handler. It is what lets the backtrace be
 * attempted at all rather than being left out as too dangerous.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The report's name inside the data directory. Fixed rather than stamped: see
   inkwell_crash_install() for why the newest crash is allowed to replace an older one. */
#define INKWELL_CRASH_REPORT_NAME "crash.txt"

/* Enough for the data directory plus the name above. A path here is a value, not an
   allocation. */
#define INKWELL_CRASH_PATH_MAX 256U

/* How much of the log the report carries, and how much of a line survives. 32 lines is about
   what a reader needs to see the last thing the process did; a line longer than this is
   invariably a dump of something rather than a sentence, and is cut with a marker. */
#define INKWELL_CRASH_LOG_LINES 32U
#define INKWELL_CRASH_LOG_LINE_MAX 160U

/* The longest a note's value may be. A screen name, a version, a link state - all short by
   construction, and a note is a fact about the process rather than anything off a wire. */
#define INKWELL_CRASH_NOTE_MAX 96U

/*
 * How many notes an application may declare, how long a label may be, and the column its value
 * is aligned to. A label shorter than the column is padded at install time, so the caller writes
 * "version" rather than counting spaces.
 */
#define INKWELL_CRASH_NOTE_SLOTS 8U
#define INKWELL_CRASH_NOTE_LABEL_MAX 12U
#define INKWELL_CRASH_NOTE_COLUMN 13U

/* The longest the caller's log warning may be. Long enough for the three or four sentences that
   are worth reading before attaching a file to a public issue, and short enough that the top of
   the report is still something somebody reads rather than scrolls. */
#define INKWELL_CRASH_WARNING_MAX 512U

/*
 * Who is crashing, and what a reader of the file needs to know about it.
 *
 * All of it is the application's, because none of it is knowable here: this layer has no name,
 * ships no binary, and has never seen the log it is about to print the tail of. Every field is
 * copied during the install.
 */
struct inkwell_crash_config {
    /* The data directory the report is written into. Required. */
    const char *dir;
    /* What to call the program in the report's own prose - "MeshClient". Required. */
    const char *product;
    /* The binary an address is resolved against, for the addr2line line. NULL uses `product`,
       which is right whenever the two are spelled the same and wrong quietly when they are
       not - so an application whose binary is lowercase should say so. */
    const char *binary;
    /* Where to send the file, printed as-is. NULL prints no such line, which is the honest
       answer for an application with nowhere to send it. */
    const char *issues_url;
    /*
     * What the log tail at the end of this report may contain, in the application's own words
     * and specific enough to act on. Required, and see the warning at the top of this header
     * for why it has no default: a generic sentence here would be this layer making a promise
     * about somebody else's log.
     */
    const char *log_warning;
    /*
     * The labels for the notes this application records, in slot order. `note_count` of them,
     * at most INKWELL_CRASH_NOTE_SLOTS, each at most INKWELL_CRASH_NOTE_LABEL_MAX characters.
     * NULL and 0 mean an application that records none.
     *
     * A fixed set of slots rather than a map of names, because the handler must not be looking
     * anything up: a slot is an index, and writing a note is a bounded copy into an array.
     */
    const char *const *note_labels;
    unsigned note_count;
};

/*
 * Install the handlers and decide where a report would go.
 *
 * The report is `config->dir` plus INKWELL_CRASH_REPORT_NAME. Returns 0, -EINVAL for a config
 * missing something required, or -errno when the handlers could not be installed - in which
 * case nothing else here does anything, which is the right failure: a program that cannot
 * report a crash is still a program.
 *
 * Calling it twice is not an error and does not stack handlers: the signal dispositions are set
 * once, and a later call re-aims the report at the directory it was given rather than quietly
 * keeping the first one. That distinction matters because the path is not the disposition - a
 * function that ignored its own argument and reported success is the failure this avoids. The
 * identity and the note labels are re-read on a second call for the same reason.
 *
 * **A report left from a previous run is read here and kept.** `inkwell_crash_report_waiting()`
 * answers from what was on disk at this moment rather than from a `stat` on demand, so a report
 * written by *this* run's own fault cannot make the running process claim it has already
 * crashed - which is a screen contradicting itself, and would be the ordinary case for anybody
 * looking at an About screen while something went wrong underneath them.
 *
 * The path is fixed, so a second crash overwrites the first. That is deliberate: the reader has
 * just watched the program die, and a file describing a fault from last week while the one they
 * are holding is thrown away would be the wrong half kept.
 */
int inkwell_crash_install(const struct inkwell_crash_config *config);

/* Where a report would be written, whether or not one is there. False when no install has
   succeeded, leaving `out` an empty string. */
bool inkwell_crash_report_path(char *out, size_t out_len);

/* Whether a report from a previous run was waiting when inkwell_crash_install() ran. */
bool inkwell_crash_report_waiting(void);

/*
 * Remove the waiting report and stop saying there is one.
 *
 * Returns 0 when the file is gone - including when it was gone already, since a discard that
 * runs twice is not a failure. This is what makes a notice resolvable: there is somewhere for it
 * to go when it is pressed.
 */
int inkwell_crash_discard(void);

/*
 * Record a fact for the next report, in the slot whose label was given at install. `value` is
 * copied; NULL or an empty string clears the slot, and a slot at or beyond
 * INKWELL_CRASH_NOTE_SLOTS is ignored.
 *
 * Safe before any install, like the log ring: the note is kept and is written out once a label
 * exists for its slot, so a fact recorded during startup is not lost to the order two calls
 * happened in. A slot that never gets a label is never written.
 *
 * Cheap enough to call on every frame - it is a bounded copy into a static buffer - which is
 * what keeps a note honest without anything having to decide when to refresh it.
 */
void inkwell_crash_note(unsigned slot, const char *value);

/*
 * Put one already-formatted log line into the ring the report will carry.
 *
 * Meant to be handed to `inkwell_log_set_sink()`, and it is the application that does the
 * handing rather than this module: log.h keeps that a decision nothing but the application
 * makes. The dependency has to run this way round either way - a handler that called into the
 * logger would be a handler using stdio on a broken process, which is the one thing this module
 * exists to avoid.
 *
 * Safe before any install: the ring is static and filling it costs a copy, so a fault during
 * startup still has the lines that led to it.
 */
void inkwell_crash_log_line(const char *line);

/*
 * Write a report for `signal_number` to `fd` exactly as the handler would, minus the fault
 * registers there is no fault to read.
 *
 * Exists for the tests, which is worth stating plainly: the handler proper cannot be called
 * directly without raising a real signal, and a suite that raised SIGSEGV in-process would be a
 * suite betting on its own handler. This is the same writer with the arch-specific half left
 * out, so the formatting, the notes and the log ring are all checked by ordinary means, and
 * only the register read and the stack walk are left to the forked case.
 */
void inkwell_crash_write_report(int fd, int signal_number);

#ifdef __cplusplus
}
#endif
