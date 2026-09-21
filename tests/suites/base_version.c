/* SemVer precedence, and the tolerances a real tag needs. */

#include "framework/inkwell_test.h"

#include "inkwell/base/version.h"

#include <stddef.h>

INKWELL_TEST_CASE(version_ordering, unit) {
    /* Each pair must compare strictly less-than in the given direction. */
    static const struct {
        const char *lower;
        const char *higher;
    } k_ordered[] = {
        {"1.0.0", "1.0.1"},
        {"1.0.9", "1.1.0"},
        {"1.9.0", "2.0.0"},
        {"1.2.0", "1.10.0"},     /* not string order */
        {"1.2.0-rc.1", "1.2.0"}, /* a prerelease precedes its release */
        {"1.2.0-beta.1", "1.2.0-beta.2"},
        {"1.2.0-beta.2", "1.2.0-beta.10"}, /* numeric identifiers compare numerically */
        {"1.2.0-beta", "1.2.0-rc"},
        {"1.2.0-rc.1", "1.2.0-rc.1.1"}, /* a longer run of identifiers outranks its prefix */
        {"garbage", "1.0.0"},           /* unparseable can never look newer */
    };
    for (size_t i = 0; i < sizeof k_ordered / sizeof k_ordered[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_version_compare(k_ordered[i].lower, k_ordered[i].higher) >= 0,
                             k_ordered[i].lower);
        INKWELL_TEST_FAIL_IF(inkwell_version_compare(k_ordered[i].higher, k_ordered[i].lower) <= 0,
                             "the reverse comparison should be positive");
    }

    /* Equality, including the forms a release tag and a build system spell differently. */
    static const char *const k_equal[][2] = {
        {"1.2.3", "1.2.3"},
        {"v1.2.3", "1.2.3"},
        {"V1.2.3", "v1.2.3"},
        {"1.2", "1.2.0"},
        {"1", "1.0.0"},
        {"1.2.3+build7", "1.2.3"}, /* build metadata is not part of precedence */
        {"1.2.3-rc.1+build7", "1.2.3-rc.1"},
    };
    for (size_t i = 0; i < sizeof k_equal / sizeof k_equal[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_version_compare(k_equal[i][0], k_equal[i][1]) != 0,
                             k_equal[i][0]);
    }

    /* Unparseable sorts below parseable, and two unparseable strings are equal - the rule that
       keeps a garbled tag off the network from ever reading as an upgrade. */
    INKWELL_TEST_FAIL_IF(inkwell_version_compare(NULL, NULL) != 0 ||
                             inkwell_version_compare("1.0.0", NULL) <= 0 ||
                             inkwell_version_compare(NULL, "1.0.0") >= 0,
                         "NULL should be handled and sort below a real version");
    INKWELL_TEST_FAIL_IF(inkwell_version_compare("garbage", "nonsense") != 0,
                         "two unparseable strings should compare equal");

    record_success(test_name);
}

INKWELL_TEST_CASE(version_prerelease, unit) {
    static const char *const k_prerelease[] = {
        "1.2.0-rc.1",
        "v1.2.0-beta.3",
        "2.0.0-alpha",
        "1.2.3-rc.1+build7",
    };
    for (size_t i = 0; i < sizeof k_prerelease / sizeof k_prerelease[0]; ++i) {
        INKWELL_TEST_FAIL_IF(!inkwell_version_is_prerelease(k_prerelease[i]), k_prerelease[i]);
    }

    /* A release, a build with only metadata, and strings this cannot read all answer false:
       the question is "does it carry a prerelease suffix", not "is it unusual". */
    static const char *const k_not[] = {"1.2.0", "v1.2.0", "1.2.3+build7", "garbage", NULL};
    for (size_t i = 0; i < sizeof k_not / sizeof k_not[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_version_is_prerelease(k_not[i]),
                             k_not[i] != NULL ? k_not[i] : "NULL");
    }

    record_success(test_name);
}

/*
 * Malformed build metadata and prerelease identifiers are unparseable, not ignorable.
 *
 * The rule this defends is the one at the top of the header: unparseable sorts below parseable,
 * so a garbled tag off a network degrades to "no update" rather than to an upgrade. Build
 * metadata takes no part in precedence, and the first version of this took that to mean it
 * needed no checking - so "2.0.0+" and "2.0.0+a..b" parsed as 2.0.0 and read as newer than a
 * running 1.x, which is precisely the direction the rule exists to prevent.
 *
 * Every case below is paired with a well-formed neighbour, because a validator that rejects
 * everything would satisfy the first half of this on its own.
 */
INKWELL_TEST_CASE(version_rejects_malformed_identifiers, unit) {
    static const char *const k_malformed[] = {
        "2.0.0+",          /* metadata introduced and then absent */
        "2.0.0+a..b",      /* an empty identifier */
        "2.0.0+.build",    /* a leading dot is the same thing */
        "2.0.0+build.",    /* and so is a trailing one */
        "2.0.0+bui|d",     /* not alphanumeric or a hyphen */
        "2.0.0-rc.1+",     /* the same tail, after a prerelease */
        "2.0.0-rc.1+a..b", /* and the same emptiness in it */
        "2.0.0-",          /* a prerelease introduced and then absent */
        "2.0.0-rc..1",     /* an empty prerelease identifier */
        "2.0.0-rc!",       /* not alphanumeric or a hyphen, in the prerelease */
    };
    for (size_t i = 0; i < sizeof k_malformed / sizeof k_malformed[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_version_compare(k_malformed[i], "1.0.0") >= 0, k_malformed[i]);
        INKWELL_TEST_FAIL_IF(inkwell_version_is_prerelease(k_malformed[i]), k_malformed[i]);
    }

    /* The well-formed neighbours, which must still parse and still outrank 1.0.0. */
    static const char *const k_fine[] = {
        "2.0.0+build.7",
        "2.0.0+21AF26D3-117B344092BD",
        "2.0.0+a-b.c-d",
        "2.0.0-rc.1+build.7",
    };
    for (size_t i = 0; i < sizeof k_fine / sizeof k_fine[0]; ++i) {
        INKWELL_TEST_FAIL_IF(inkwell_version_compare(k_fine[i], "1.0.0") <= 0, k_fine[i]);
    }
    /* And the metadata still takes no part in precedence once it is well formed. */
    INKWELL_TEST_FAIL_IF(inkwell_version_compare("2.0.0+build.7", "2.0.0+other.9") != 0,
                         "build metadata must not order two otherwise equal versions");
    record_success(test_name);
}
