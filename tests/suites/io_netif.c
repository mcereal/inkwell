#define _GNU_SOURCE

/*
 * Network interfaces. Off Linux, every call refuses. On Linux as root, the real thing in a
 * network namespace of the test's own (a child, so the machine's network is never touched):
 * loopback brought up, an address put on it and taken off, a default route through it, the DHCP
 * client's socket bound to it, and the monitor hearing each change.
 */
#include "framework/inkwell_test.h"

#include "inkwell/io/netif.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include "inkwell/runtime/loop.h"

#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(__linux__)

INKWELL_TEST_CASE(netif_refuses_off_linux, unit) {
    struct inkwell_netif_link link;
    INKWELL_TEST_FAIL_IF(inkwell_netif_links(&link, 1U) != -ENOSYS, "no links to list");
    INKWELL_TEST_FAIL_IF(inkwell_netif_set_up(1, true) != -ENOSYS, "nothing to bring up");
    INKWELL_TEST_FAIL_IF(inkwell_netif_dhcp_socket("en0") != -ENOSYS, "no DHCP socket");
    record_success(test_name);
}

#else

static unsigned g_changed;

static void on_change(unsigned changed, void *userdata) {
    (void)userdata;
    g_changed |= changed;
}

static void settle(struct inkwell_loop *loop) {
    for (int i = 0; i < 20; ++i) {
        (void)inkwell_loop_run(loop, 5);
    }
}

/* The child's whole run; its exit status is the step that failed, 0 for none. */
static int in_a_namespace(void) {
    if (unshare(CLONE_NEWNET) != 0) {
        return 100; /* not allowed here: skipped */
    }
    struct inkwell_netif_link lo;
    if (inkwell_netif_link_named("lo", &lo) != 0 || !lo.loopback || lo.up) {
        return 1; /* a new namespace has loopback, down */
    }
    struct inkwell_loop loop;
    struct inkwell_netif_monitor monitor;
    memset(&monitor, 0, sizeof monitor);
    if (inkwell_loop_init(&loop) != 0 ||
        inkwell_netif_monitor_open(&monitor, &loop, on_change, NULL) != 0) {
        return 2;
    }
    if (inkwell_netif_set_up(lo.index, true) != 0) {
        return 3;
    }
    settle(&loop);
    if ((g_changed & INKWELL_NETIF_LINKS) == 0U || inkwell_netif_link_named("lo", &lo) != 0 ||
        !lo.up) {
        return 4; /* up, and heard */
    }
    const uint8_t address[4] = {10, 9, 9, 1}, gateway[4] = {10, 9, 9, 2};
    g_changed = 0U;
    if (inkwell_netif_address_add(lo.index, address, 24U) != 0 ||
        inkwell_netif_address_add(lo.index, address, 24U) != 0) {
        return 5; /* added, and adding it again is no error */
    }
    struct inkwell_netif_address found[8];
    const int n = inkwell_netif_addresses(lo.index, found, 8U);
    bool have = false;
    for (int i = 0; i < n && i < 8; ++i) {
        have = have || (memcmp(found[i].address, address, 4) == 0 && found[i].prefix == 24U);
    }
    settle(&loop);
    if (!have || (g_changed & INKWELL_NETIF_ADDRESSES) == 0U) {
        return 6;
    }
    if (inkwell_netif_route_default(lo.index, gateway) != 0) {
        return 7;
    }
    const int fd = inkwell_netif_dhcp_socket("lo");
    if (fd < 0) {
        return 8;
    }
    (void)close(fd);
    if (inkwell_netif_address_flush(lo.index) != 0) {
        return 9;
    }
    struct inkwell_netif_address left[8];
    int remaining = 0;
    const int after = inkwell_netif_addresses(lo.index, left, 8U);
    for (int i = 0; i < after && i < 8; ++i) {
        remaining += left[i].address[0] == 10U;
    }
    if (remaining != 0) {
        return 10;
    }
    if (inkwell_netif_link_named("nothing0", &lo) != -ENODEV) {
        return 11;
    }
    inkwell_netif_monitor_close(&monitor);
    inkwell_loop_shutdown(&loop);
    return 0;
}

INKWELL_TEST_CASE(netif_changes_links_addresses_and_routes, unit) {
    struct inkwell_netif_link links[8];
    INKWELL_TEST_FAIL_IF(inkwell_netif_links(links, 8U) < 1, "this namespace has at least lo");
    if (geteuid() != 0) {
        printf("  (changes skipped: needs root)\n");
        record_success(test_name);
        return;
    }
    const pid_t child = fork();
    if (child == 0) {
        _exit(in_a_namespace());
    }
    int status = 0;
    (void)waitpid(child, &status, 0);
    const int step = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
    if (step == 100) {
        printf("  (changes skipped: no network namespace here)\n");
        record_success(test_name);
        return;
    }
    char why[64];
    snprintf(why, sizeof why, "failed at step %d (see in_a_namespace)", step);
    INKWELL_TEST_FAIL_IF(step != 0, why);
    record_success(test_name);
}

#endif
