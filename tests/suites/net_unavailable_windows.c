#include "framework/inkwell_test.h"

#include "inkwell/net/fetch.h"
#include "inkwell/net/tls.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>

static void fetch_done(void *userdata, const struct inkwell_fetch_result *result) {
    bool *called = (bool *)userdata;
    (void)result;
    *called = true;
}

INKWELL_TEST_CASE(net_windows_fetch_refuses_without_a_backend, unit) {
    struct inkwell_loop loop = {0};
    struct inkwell_fetch fetch;
    bool called = false;
    const struct inkwell_fetch_request request = {
        .url = "https://example.com/", .on_done = fetch_done, .userdata = &called};
    INKWELL_TEST_FAIL_IF(inkwell_fetch_init(&fetch, &loop) != 0, "fetch init should succeed");
    inkwell_fetch_connect_to(&fetch, "127.0.0.1", 443U);
    const int started = inkwell_fetch_start(&fetch, &request, 0U);
    const bool unavailable = !inkwell_fetch_available(&fetch) && !inkwell_fetch_busy(&fetch) &&
                             started == -ENOTSUP && !called && fetch.conn == NULL;
    inkwell_fetch_tick(&fetch, 42U);
    inkwell_fetch_cancel(&fetch);
    inkwell_fetch_shutdown(&fetch);
    INKWELL_TEST_FAIL_IF(!unavailable, "Windows fetch should refuse without starting a request");
    record_success(test_name);
}

INKWELL_TEST_CASE(net_windows_tls_refuses_even_when_requested, unit) {
    struct inkwell_tls_client tls;
    const int started = inkwell_tls_client_start(&tls, -1, "example.com", NULL);
    INKWELL_TEST_FAIL_IF(inkwell_tls_available() || started != -ENOTSUP,
                         "Windows should never expose the POSIX TLS socket backend");
    inkwell_tls_client_stop(&tls);
    record_success(test_name);
}
