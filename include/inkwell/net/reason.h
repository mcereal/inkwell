#pragma once

/*
 * Why a connection attempt did not end in a connection.
 *
 * Every transport that opens a socket to a named host fails in the same handful of ways, and
 * until now each one wrote those ways down again in its own vocabulary - as an id into the
 * application's string catalog, which is the one thing a platform layer must not deal in. A
 * layer that hands back a word owns the sentence, the language and the tone of every
 * application above it, and none of those are its to own.
 *
 * So a failure here is a *reason* and a number, and the application turns the pair into
 * whatever it wants to say. The division is the same one inkwell_resolve already draws with
 * `enum inkwell_resolve_outcome`: this is that idea widened from the lookup to the whole path
 * from a typed target to a stream.
 *
 * **Nothing collapses here.** Two reasons stay apart whenever the layer can actually tell them
 * apart, even where every application today says the same thing about both - a name that does
 * not resolve and a resolver that timed out are one sentence in most UIs and two different
 * things to do about it, and an application that wants the distinction cannot invent it back
 * once this enum has thrown it away. Collapsing is a decision about words, so it belongs in the
 * table that produces words.
 *
 * **The subject is not in here.** Which host, which port, which target the user typed - the
 * caller passed all of that in and still has it. What comes back is only what the caller could
 * not have known.
 */

#include "inkwell/net/resolve.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What went wrong, at the grain the layer can actually distinguish.
 *
 * `detail` on the failure below carries the number behind the reason, and which number that is
 * depends on the reason - it is noted on each.
 */
enum inkwell_net_reason {
    /* No failure. A zero-initialised failure record is "nothing has gone wrong yet", which is
       what lets one be cleared with a memset rather than with a constant somebody has to know. */
    INKWELL_NET_OK = 0,
    /* The target is not a host and port this could parse at all. Nothing was attempted; the
       string the caller holds is the whole story. */
    INKWELL_NET_BAD_ADDRESS,
    /* The name resolved to nothing: NXDOMAIN, or a name with no address record. */
    INKWELL_NET_UNKNOWN_HOST,
    /* The lookup itself did not work - no resolver, no route to one, the child could not be
       read. Says nothing about whether the name exists. `detail` is the `EAI_*` code, or 0. */
    INKWELL_NET_LOOKUP_FAILED,
    /* The lookup was still running when its deadline passed. Kept apart from LOOKUP_FAILED
       above on purpose: see the note about collapsing. */
    INKWELL_NET_LOOKUP_TIMED_OUT,
    /* The socket could not be opened, or the connect was refused or could not be routed.
       `detail` is the negative errno. */
    INKWELL_NET_UNREACHABLE,
    /* Something was begun and never finished inside its deadline - a connect that stayed
       half-open, a handshake with no reply. The far end may be there and may simply be slow. */
    INKWELL_NET_TIMED_OUT,
    /* The peer closed the connection, at any point after it was established. */
    INKWELL_NET_CLOSED,
    /* The TLS handshake failed. `detail` is the library's own code, which is worth logging and
       not worth showing - it is the same class of thing as an `EAI_*`. */
    INKWELL_NET_TLS,
    INKWELL_NET_REASON_COUNT,
};

/*
 * One failure, waiting to be reported.
 *
 * Two fields and no buffer, which is the point: this is what a transport keeps instead of a
 * formatted sentence, so the sentence is built once, by whoever knows what language the reader
 * has, at the moment somebody actually asks.
 */
struct inkwell_net_failure {
    enum inkwell_net_reason reason;
    /* The number behind the reason - a negative errno, an `EAI_*`, a TLS code - or 0 where the
       reason says everything there is to say. See each reason above. */
    int detail;
};

/* True when a failure is recorded. Spelled as a call rather than as `f.reason != INKWELL_NET_OK`
   at every call site, because that comparison is the kind that gets written the other way round
   once. */
static inline bool inkwell_net_failed(const struct inkwell_net_failure *failure) {
    return failure != NULL && failure->reason != INKWELL_NET_OK;
}

/*
 * The reason an errno from socket(), connect() or SO_ERROR stands for.
 *
 * Mostly UNREACHABLE, because most of them are: a refused connection, no route, an unreachable
 * network and an address that cannot be assigned are four kernel answers to one question a user
 * asked, and no screen has ever usefully told them apart. The two that are not are worth
 * separating - a connect that timed out is a far end that may be there, and a reset is a far end
 * that certainly was.
 *
 * `err` may be given positive or negative; both spellings appear at a call site depending on
 * whether it came from `errno` or from a return value.
 */
enum inkwell_net_reason inkwell_net_reason_from_errno(int err);

/* The reason a finished lookup stands for. INKWELL_RESOLVE_OK maps to INKWELL_NET_OK, so a
   caller may hand over any outcome rather than checking for success first. */
enum inkwell_net_reason inkwell_net_reason_from_resolve(enum inkwell_resolve_outcome outcome);

/*
 * A short ASCII name for a reason - "unreachable", "unknown-host" - for a log line and for a
 * test that would otherwise assert on an integer.
 *
 * This is not the exception to the rule at the top of this file. inkwell's log is English and
 * deliberately untranslated (a bug report in a language the maintainer cannot read is worse
 * than no bug report), and these are symbol names rather than prose, in the same class as
 * `strerror()` output. Nothing here is fit to put on a screen and none of it is a sentence.
 */
const char *inkwell_net_reason_name(enum inkwell_net_reason reason);

#ifdef __cplusplus
}
#endif
