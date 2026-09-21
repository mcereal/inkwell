#pragma once

/*
 * Semantic version strings, ordered.
 *
 * This is the arithmetic only: two strings in, an ordering out. It holds no version of its own
 * and has no opinion about which build is running - that is the application's, because only the
 * application's build system knows what it stamped. The seam is deliberate. A self-updater asks
 * two separate questions - "is this tag newer than what I am?" and "am I a build that may be
 * replaced at all?" - and only the first is arithmetic. Folding the second in here would mean
 * this file reading a compile definition it cannot own, and every consumer inheriting whichever
 * one it picked.
 *
 * SemVer 2.0.0 precedence, with the tolerances a real tag needs: a leading 'v' or 'V' (a GitHub
 * tag carries one, a build system does not), and a missing minor or patch ("1.2" == "1.2.0").
 * Build metadata after '+' takes no part in precedence, as the spec requires.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Orders two versions: negative when a < b, 0 when equal, positive when a > b.
 *
 * A prerelease sorts below the release it precedes ("1.2.0-rc.1" < "1.2.0"), and two
 * prereleases are compared dot-part by dot-part with numeric parts ordered numerically - which
 * is what keeps a beta and an rc channel from offering to move each other sideways.
 *
 * **Anything unparseable compares as lower than anything parseable**, and two unparseable
 * strings compare equal. That is the rule that matters for a caller reading tags off a network:
 * a garbled one can never look newer than what is running, so a malformed answer degrades to
 * "no update" rather than to an upgrade to nothing.
 */
int inkwell_version_compare(const char *a, const char *b);

/* True when `version` parses and carries a prerelease suffix - the '-rc.1' of "1.2.0-rc.1".
   False for a release, and false for anything this cannot read. */
bool inkwell_version_is_prerelease(const char *version);

#ifdef __cplusplus
}
#endif
