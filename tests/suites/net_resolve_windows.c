#include "framework/inkwell_test.h"

#include "inkwell/net/resolve.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <string.h>

struct resolve_probe {
    unsigned calls;
    struct inkwell_resolve_result result;
};

static void record_done(void *userdata, const struct inkwell_resolve_result *result) {
    struct resolve_probe *probe = (struct resolve_probe *)userdata;
    probe->calls++;
    probe->result = *result;
}

static bool pump_until_done(struct inkwell_loop *loop, struct inkwell_resolve *resolve,
                            struct resolve_probe *probe) {
    for (unsigned turn = 0U; turn < 100U && probe->calls == 0U; ++turn) {
        (void)inkwell_loop_run(loop, 50);
        inkwell_resolve_tick(resolve, (uint64_t)turn * 50U);
    }
    return probe->calls > 0U;
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

INKWELL_TEST_CASE(resolve_windows_without_a_loop_is_unavailable, unit) {
    struct inkwell_resolve resolve;
    INKWELL_TEST_FAIL_IF(inkwell_resolve_init(&resolve, NULL) != 0, "init should succeed");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_available(&resolve),
                         "a resolver without a loop should be unavailable");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_start(&resolve, "example.invalid", 4403U, record_done,
                                               NULL, 0U) != -ENOTSUP,
                         "hostname lookup without a loop should refuse");
    INKWELL_TEST_FAIL_IF(inkwell_resolve_busy(&resolve), "refusal should not start work");
    inkwell_resolve_shutdown(&resolve);
    record_success(test_name);
}

INKWELL_TEST_CASE(resolve_windows_finds_a_name_asynchronously, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop should start");
    struct inkwell_resolve resolve;
    (void)inkwell_resolve_init(&resolve, &loop);
    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);

    const int started =
        inkwell_resolve_start(&resolve, "localhost", 4403U, record_done, &probe, 0U);
    INKWELL_TEST_FAIL_IF_CLEANUP(started != 0,
                                 (inkwell_resolve_shutdown(&resolve), inkwell_loop_shutdown(&loop)),
                                 "lookup should start");
    INKWELL_TEST_FAIL_IF_CLEANUP(probe.calls != 0U || !inkwell_resolve_busy(&resolve),
                                 (inkwell_resolve_shutdown(&resolve), inkwell_loop_shutdown(&loop)),
                                 "start should return before the lookup reports");
    INKWELL_TEST_FAIL_IF_CLEANUP(!pump_until_done(&loop, &resolve, &probe),
                                 (inkwell_resolve_shutdown(&resolve), inkwell_loop_shutdown(&loop)),
                                 "lookup should report through the loop");
    const bool good = probe.calls == 1U && probe.result.outcome == INKWELL_RESOLVE_OK &&
                      probe.result.address_count > 0U && probe.result.address_len != 0 &&
                      !inkwell_resolve_busy(&resolve);
    inkwell_resolve_shutdown(&resolve);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(!good, "localhost should resolve once with a usable address");
    record_success(test_name);
}

INKWELL_TEST_CASE(resolve_windows_cancel_is_silent_and_reusable, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop should start");
    struct inkwell_resolve resolve;
    (void)inkwell_resolve_init(&resolve, &loop);
    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);

    const int started =
        inkwell_resolve_start(&resolve, "localhost", 4403U, record_done, &probe, 0U);
    const int duplicate =
        inkwell_resolve_start(&resolve, "localhost", 4403U, record_done, &probe, 0U);
    inkwell_resolve_cancel(&resolve);
    const bool cancelled =
        started == 0 && duplicate == -EBUSY && probe.calls == 0U && !inkwell_resolve_busy(&resolve);
    const int restarted =
        inkwell_resolve_start(&resolve, "localhost", 4403U, record_done, &probe, 0U);
    const bool finished = restarted == 0 && pump_until_done(&loop, &resolve, &probe);
    inkwell_resolve_shutdown(&resolve);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(!cancelled || !finished || probe.calls != 1U,
                         "cancellation should be silent and leave the resolver reusable");
    record_success(test_name);
}
