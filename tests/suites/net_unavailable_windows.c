#include "framework/inkwell_test.h"

#include "inkwell/net/fetch.h"
#include "inkwell/net/mqtt.h"
#include "inkwell/net/tls.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <string.h>

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

INKWELL_TEST_CASE(net_windows_mqtt_refuses_without_a_backend, unit) {
    struct inkwell_loop loop = {0};
    struct inkwell_mqtt_client proxy;
    struct inkwell_mqtt_client_config config = {0};
    memcpy(config.address, "127.0.0.1", sizeof "127.0.0.1");
    memcpy(config.client_id, "window-test", sizeof "window-test");
    INKWELL_TEST_FAIL_IF(inkwell_mqtt_client_init(&proxy, &loop) != 0, "MQTT init should succeed");
    memcpy(config.address, "host:", sizeof "host:");
    const int bad_port = inkwell_mqtt_client_start(&proxy, &config, NULL, NULL, NULL, 0U);
    memcpy(config.address, "[not-an-address", sizeof "[not-an-address");
    const int bad_bracket = inkwell_mqtt_client_start(&proxy, &config, NULL, NULL, NULL, 0U);
    memcpy(config.address, "127.0.0.1", sizeof "127.0.0.1");
    const int started = inkwell_mqtt_client_start(&proxy, &config, NULL, NULL, NULL, 0U);
    const int subscribed = inkwell_mqtt_client_subscribe(&proxy, "test/#");
    const int duplicate = inkwell_mqtt_client_subscribe(&proxy, "test/#");
    const int published = inkwell_mqtt_client_publish(&proxy, "test/one", NULL, 0U, false);
    const bool unavailable = bad_port == -EINVAL && bad_bracket == -EINVAL && started == -ENOTSUP &&
                             subscribed == 0 && duplicate == -EEXIST && published == -ENOTCONN &&
                             !inkwell_mqtt_client_is_ready(&proxy) &&
                             inkwell_mqtt_client_state(&proxy) == INKWELL_MQTT_CLIENT_OFF &&
                             inkwell_mqtt_client_stats(&proxy).dropped == 1U;
    inkwell_mqtt_client_clear_filters(&proxy);
    inkwell_mqtt_client_shutdown(&proxy);
    INKWELL_TEST_FAIL_IF(!unavailable,
                         "Windows MQTT should remain off and count a dropped publish");
    record_success(test_name);
}
