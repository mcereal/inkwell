#define _POSIX_C_SOURCE 200809L

#include "inkwell/base/env.h"

#include "inkwell/base/log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define INKWELL_ENV_PREFIX_MAX 32U
#define INKWELL_ENV_NAME_MAX 128U

static char g_prefix[INKWELL_ENV_PREFIX_MAX] = "INKWELL";

void inkwell_env_set_prefix(const char *prefix) {
    if (prefix == NULL || prefix[0] == '\0') {
        (void)snprintf(g_prefix, sizeof g_prefix, "%s", "INKWELL");
        return;
    }
    (void)snprintf(g_prefix, sizeof g_prefix, "%s", prefix);
}

const char *inkwell_env_prefix(void) {
    return g_prefix;
}

const char *inkwell_env_get(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    char full[INKWELL_ENV_NAME_MAX];
    (void)snprintf(full, sizeof full, "%s_%s", g_prefix, name);
    return getenv(full);
}

static const char *const kTrue[] = {"1", "true", "yes", "on"};
static const char *const kFalse[] = {"0", "false", "no", "off"};

static bool matches_any(const char *value, const char *const *words, size_t count) {
    for (size_t i = 0U; i < count; ++i) {
        if (strcasecmp(value, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool inkwell_env_bool(const char *name, const char *label, bool fallback) {
    const char *value = inkwell_env_get(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    if (matches_any(value, kTrue, sizeof kTrue / sizeof kTrue[0])) {
        return true;
    }
    if (matches_any(value, kFalse, sizeof kFalse / sizeof kFalse[0])) {
        return false;
    }
    inkwell_log_warn("env", "%s='%s' is not a boolean; keeping %s %s", name, value,
                     label != NULL ? label : name, fallback ? "on" : "off");
    return fallback;
}

long inkwell_env_int(const char *name, long min, long max, long fallback) {
    const char *value = inkwell_env_get(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno == ERANGE || parsed < min || parsed > max) {
        inkwell_log_warn("env", "%s='%s' is not an integer in %ld..%ld; using %ld", name, value,
                         min, max, fallback);
        return fallback;
    }
    return parsed;
}
