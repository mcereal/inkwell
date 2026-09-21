/* The failure vocabulary a transport reports in, and the two mappings into it. */

#include "framework/inkwell_test.h"

#include "inkwell/net/reason.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

INKWELL_TEST_CASE(net_reason_errno_mapping, unit) {
    static const struct {
        int err;
        enum inkwell_net_reason reason;
    } k_cases[] = {
        {0, INKWELL_NET_OK},
        {ECONNREFUSED, INKWELL_NET_UNREACHABLE},
        {EHOSTUNREACH, INKWELL_NET_UNREACHABLE},
        {ENETUNREACH, INKWELL_NET_UNREACHABLE},
        {EACCES, INKWELL_NET_UNREACHABLE},
        {ETIMEDOUT, INKWELL_NET_TIMED_OUT},
        {ECONNRESET, INKWELL_NET_CLOSED},
        {EPIPE, INKWELL_NET_CLOSED},
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_errno(k_cases[i].err) != k_cases[i].reason,
                             inkwell_net_reason_name(k_cases[i].reason));
        /* Both spellings of the same error, because both appear at a call site: one came from
           `errno` and the other off a return value. */
        INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_errno(-k_cases[i].err) != k_cases[i].reason,
                             "the negative spelling should map the same way");
    }
    record_success(test_name);
}

INKWELL_TEST_CASE(net_reason_resolve_mapping, unit) {
    INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_resolve(INKWELL_RESOLVE_OK) != INKWELL_NET_OK,
                         "a successful lookup is not a failure");
    INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_resolve(INKWELL_RESOLVE_NOT_FOUND) !=
                             INKWELL_NET_UNKNOWN_HOST,
                         "NXDOMAIN is an unknown host");
    /*
     * The two the application is free to say one thing about and the layer is not free to
     * merge. A resolver that timed out may be answering about a name that is perfectly good,
     * and a caller that wants to retry one and not the other cannot invent the difference back
     * once it has been thrown away here.
     */
    INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_resolve(INKWELL_RESOLVE_TIMED_OUT) !=
                             INKWELL_NET_LOOKUP_TIMED_OUT,
                         "a lookup that timed out is not a lookup that failed");
    INKWELL_TEST_FAIL_IF(inkwell_net_reason_from_resolve(INKWELL_RESOLVE_FAILED) !=
                             INKWELL_NET_LOOKUP_FAILED,
                         "a resolver that did not work is LOOKUP_FAILED");
    record_success(test_name);
}

INKWELL_TEST_CASE(net_reason_every_reason_is_named, unit) {
    /*
     * A name per reason, all distinct, none empty. The loop is over the enum rather than over a
     * list of the ones somebody remembered, so adding a reason without naming it fails here
     * instead of printing "unknown" in a log six months later.
     */
    for (int i = 0; i < (int)INKWELL_NET_REASON_COUNT; ++i) {
        const char *const name = inkwell_net_reason_name((enum inkwell_net_reason)i);
        INKWELL_TEST_FAIL_IF(name == NULL || name[0] == '\0', "every reason needs a name");
        INKWELL_TEST_FAIL_IF(strcmp(name, "unknown") == 0, "a named reason is not 'unknown'");
        for (int j = 0; j < i; ++j) {
            const char *const earlier = inkwell_net_reason_name((enum inkwell_net_reason)j);
            INKWELL_TEST_FAIL_IF(strcmp(name, earlier) == 0, "two reasons share a name");
        }
    }
    const char *const out_of_range = inkwell_net_reason_name((enum inkwell_net_reason)999);
    INKWELL_TEST_FAIL_IF(strcmp(out_of_range, "unknown") != 0,
                         "an out-of-range reason still returns a string");
    record_success(test_name);
}

INKWELL_TEST_CASE(net_reason_failure_record_starts_clear, unit) {
    /* Zero is "nothing has gone wrong", which is what lets a caller clear one with a memset
       rather than with a constant it has to know the name of. */
    struct inkwell_net_failure failure;
    memset(&failure, 0, sizeof failure);
    INKWELL_TEST_FAIL_IF(inkwell_net_failed(&failure), "a zeroed failure is not a failure");
    INKWELL_TEST_FAIL_IF(inkwell_net_failed(NULL), "no record is not a failure either");

    failure.reason = INKWELL_NET_UNREACHABLE;
    failure.detail = -ECONNREFUSED;
    INKWELL_TEST_FAIL_IF(!inkwell_net_failed(&failure), "a recorded reason is a failure");
    record_success(test_name);
}
