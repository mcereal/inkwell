#include "framework/inkwell_test.h"

#include "inkwell/net/resolve.h"

#include <errno.h>

static void unused_done(void *userdata, const struct inkwell_resolve_result *result) {
    (void)userdata;
    (void)result;
}

INKWELL_TEST_CASE(resolve_windows_literals, unit) {
    struct sockaddr_storage address;
    socklen_t len = 0;
    INKWELL_TEST_FAIL_IF(!inkwell_resolve_literal("192.0.2.1", 4403U, &address, &len),
                         "IPv4 literal should parse");
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)&address;
    INKWELL_TEST_FAIL_IF(v4->sin_family != AF_INET || ntohs(v4->sin_port) != 4403U ||
                             len != (socklen_t)sizeof *v4,
                         "IPv4 literal should include the port");
    INKWELL_TEST_FAIL_IF(!inkwell_resolve_literal("2001:db8::1", 4404U, &address, &len),
                         "IPv6 literal should parse");
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&address;
    INKWELL_TEST_FAIL_IF(v6->sin6_family != AF_INET6 || ntohs(v6->sin6_port) != 4404U ||
                             len != (socklen_t)sizeof *v6,
                         "IPv6 literal should include the port");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_literal("192.0.2.1x", 4403U, &address, &len),
                         "malformed IPv4 should not parse");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_literal("example.invalid", 4403U, &address, &len),
                         "a hostname is not a literal");
    record_success(test_name);
}

INKWELL_TEST_CASE(resolve_windows_names_refused_without_blocking, unit) {
    struct inkwell_resolve resolve;
    INKWELL_TEST_FAIL_IF(inkwell_resolve_init(&resolve, NULL) != 0, "init should succeed");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_available(&resolve),
                         "asynchronous lookup is not yet available");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_start(&resolve, "example.invalid", 4403U, unused_done,
                                               NULL, 0U) != -ENOTSUP,
                         "hostname lookup should refuse instead of blocking");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_busy(&resolve), "refusal should not start work");
    inkwell_resolve_shutdown(&resolve);
    record_success(test_name);
}
