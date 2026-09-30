#include "framework/inkwell_test.h"

#include "inkwell/runtime/crash.h"

#include <errno.h>

INKWELL_TEST_CASE(crash_windows_discard_is_idempotent_without_a_backend, unit) {
    const struct inkwell_crash_config config = {
        .dir = ".",
        .product = "TestApp",
        .log_warning = "Check the log before sharing.",
    };
    INKWELL_TEST_FAIL_IF(inkwell_crash_install(&config) != -ENOTSUP,
                         "Windows crash installation should remain unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_crash_discard() != 0, "discard without a report should succeed");
    INKWELL_TEST_FAIL_IF(inkwell_crash_discard() != 0, "repeated discard should succeed");
    record_success(test_name);
}
