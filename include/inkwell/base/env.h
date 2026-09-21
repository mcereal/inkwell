#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One reading of the environment knobs, under the application's own prefix.
 *
 * These are the documented escape hatches that people reach for on a device with no keyboard,
 * so "<PREFIX>_AUTOCONNECT=false" and "<PREFIX>_DISABLE_BLE=no" have to mean what they look
 * like. Three call sites had each grown their own parser, and they disagreed: one accepted
 * "off" but not "yes", another the reverse, a third only "0". Anything a user typed that landed
 * in the gap was silently read as the opposite of what they meant.
 *
 * Every name below is a *suffix*: inkwell reads "THEME" as "<PREFIX>_THEME", so the library's
 * own knobs and the application's are one namespace rather than two, and the same binary
 * shipped under a different name does not answer to the old one's variables.
 */

/*
 * The prefix, without its underscore. "INKWELL" until an application says otherwise, which it
 * should do once, before anything else reads a knob.
 *
 * The string is copied, so a caller may pass a temporary. An empty or NULL prefix restores the
 * default rather than reading bare names - a library that answered to "THEME" would be reading
 * whatever the shell happened to have.
 */
void inkwell_env_set_prefix(const char *prefix);
const char *inkwell_env_prefix(void);

/* The raw value of "<PREFIX>_<name>", or NULL when it is unset. */
const char *inkwell_env_get(const char *name);

/* Truthy: "1", "true", "yes", "on". Falsy: "0", "false", "no", "off". Case-insensitive.
   Unset, empty and unrecognised all yield `fallback`; an unrecognised value also warns, since
   it means the user asked for something and did not get it. `label` names the knob in that
   warning (e.g. "BLE"), and may be NULL to use the variable's own name. */
bool inkwell_env_bool(const char *name, const char *label, bool fallback);

/* Decimal integer within [min, max]. Unset and empty yield `fallback` quietly; a value that is
   not a number, or is out of range, yields `fallback` and warns. */
long inkwell_env_int(const char *name, long min, long max, long fallback);

#ifdef __cplusplus
}
#endif
