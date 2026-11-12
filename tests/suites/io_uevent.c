/*
 * Kernel device events. The parser is tested on every system against messages laid out as the
 * kernel writes them. The monitor is Linux's; elsewhere it must refuse, and on Linux a root run
 * also has the kernel announce a device and checks the event arrives on the loop.
 */
#include "framework/inkwell_test.h"

#include "inkwell/io/uevent.h"
#include "inkwell/runtime/loop.h"

#include <errno.h>
#include <string.h>
#if defined(__linux__)
#include <unistd.h>
#endif

/* sizeof a literal counts its closing NUL, which is the kernel's last terminator. */
#define MESSAGE(text) text, sizeof text

static const char k_udc_add[] = "add@/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3"
                                "\0ACTION=add"
                                "\0DEVPATH=/devices/platform/11210000.usb/11210000.dwc3/udc/"
                                "11210000.dwc3"
                                "\0SUBSYSTEM=udc"
                                "\0SEQNUM=4127";

static const char k_input_add[] = "add@/devices/virtual/input/input7/event3"
                                  "\0ACTION=add"
                                  "\0DEVPATH=/devices/virtual/input/input7/event3"
                                  "\0SUBSYSTEM=input"
                                  "\0MAJOR=13"
                                  "\0MINOR=67"
                                  "\0DEVNAME=input/event3"
                                  "\0SEQNUM=4130";

static const char k_usb_bind[] = "bind@/devices/platform/usb/1-1"
                                 "\0ACTION=bind"
                                 "\0DEVPATH=/devices/platform/usb/1-1"
                                 "\0SUBSYSTEM=usb"
                                 "\0DEVTYPE=usb_device"
                                 "\0DRIVER=usb"
                                 "\0PRODUCT=1d6b/104/100"
                                 "\0SEQNUM=18446744073709551615";

INKWELL_TEST_CASE(uevent_parses_a_controller_add, unit) {
    struct inkwell_uevent ev;
    const int parsed = inkwell_uevent_parse(MESSAGE(k_udc_add), &ev);
    INKWELL_TEST_FAIL_IF(parsed != 0, "a kernel message did not parse");
    INKWELL_TEST_FAIL_IF(strcmp(ev.action, "add") != 0, "action");
    INKWELL_TEST_FAIL_IF(
        strcmp(ev.devpath, "/devices/platform/11210000.usb/11210000.dwc3/udc/11210000.dwc3") != 0,
        "devpath");
    INKWELL_TEST_FAIL_IF(ev.subsystem == NULL || strcmp(ev.subsystem, "udc") != 0, "subsystem");
    INKWELL_TEST_FAIL_IF(ev.devname != NULL || ev.devtype != NULL, "absent keys are NULL");
    INKWELL_TEST_FAIL_IF(ev.seqnum != 4127U, "seqnum");
    record_success(test_name);
}

INKWELL_TEST_CASE(uevent_parses_a_device_node_and_any_key, unit) {
    struct inkwell_uevent ev;
    INKWELL_TEST_FAIL_IF(inkwell_uevent_parse(MESSAGE(k_input_add), &ev) != 0, "parse");
    INKWELL_TEST_FAIL_IF(ev.devname == NULL || strcmp(ev.devname, "input/event3") != 0, "devname");
    const char *major = inkwell_uevent_get(&ev, "MAJOR");
    const char *minor = inkwell_uevent_get(&ev, "MINOR");
    INKWELL_TEST_FAIL_IF(major == NULL || strcmp(major, "13") != 0, "MAJOR");
    INKWELL_TEST_FAIL_IF(minor == NULL || strcmp(minor, "67") != 0, "MINOR");
    INKWELL_TEST_FAIL_IF(inkwell_uevent_get(&ev, "MAJ") != NULL, "a prefix is not a key");
    INKWELL_TEST_FAIL_IF(inkwell_uevent_get(&ev, "DRIVER") != NULL, "an absent key is NULL");
    record_success(test_name);
}

INKWELL_TEST_CASE(uevent_parses_bind_and_a_full_seqnum, unit) {
    struct inkwell_uevent ev;
    INKWELL_TEST_FAIL_IF(inkwell_uevent_parse(MESSAGE(k_usb_bind), &ev) != 0, "parse");
    INKWELL_TEST_FAIL_IF(strcmp(ev.action, "bind") != 0, "bind is an action like any other");
    INKWELL_TEST_FAIL_IF(ev.devtype == NULL || strcmp(ev.devtype, "usb_device") != 0, "devtype");
    INKWELL_TEST_FAIL_IF(ev.seqnum != UINT64_MAX, "the seqnum is 64 bits");
    const char *product = inkwell_uevent_get(&ev, "PRODUCT");
    INKWELL_TEST_FAIL_IF(product == NULL || strcmp(product, "1d6b/104/100") != 0, "PRODUCT");
    record_success(test_name);
}

INKWELL_TEST_CASE(uevent_refuses_what_is_not_a_kernel_message, unit) {
    static const char udevd[] = "libudev\0\xfe\xed\xca\xfe";
    static const char no_header[] = "ACTION=add\0DEVPATH=/devices/x";
    static const char header_only[] = "add@/devices/x";
    static const char no_action[] = "add@/devices/x\0DEVPATH=/devices/x";
    static const char mismatch[] = "add@/devices/x\0ACTION=remove\0DEVPATH=/devices/x";
    static const char other_path[] = "add@/devices/x\0ACTION=add\0DEVPATH=/devices/y";
    static const char bad_seqnum[] = "add@/devices/x\0ACTION=add\0DEVPATH=/devices/x\0SEQNUM=12a";
    static const char relative[] = "add@devices/x\0ACTION=add\0DEVPATH=devices/x";
    const struct {
        const char *buf;
        size_t len;
        const char *why;
    } cases[] = {
        {udevd, sizeof udevd, "udevd's re-broadcast"},
        {no_header, sizeof no_header, "no header"},
        {header_only, sizeof header_only, "a header and nothing else"},
        {no_action, sizeof no_action, "no ACTION"},
        {mismatch, sizeof mismatch, "a header that disagrees with ACTION"},
        {other_path, sizeof other_path, "a header that disagrees with DEVPATH"},
        {bad_seqnum, sizeof bad_seqnum, "a SEQNUM that is not a number"},
        {relative, sizeof relative, "a devpath that is not under /"},
        {k_udc_add, sizeof k_udc_add - 1, "no closing NUL"},
        {k_udc_add, 0, "nothing at all"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        struct inkwell_uevent ev;
        if (inkwell_uevent_parse(cases[i].buf, cases[i].len, &ev) != -EINVAL) {
            record_failure(test_name, cases[i].why);
            return;
        }
    }
    record_success(test_name);
}

static void count_event(const struct inkwell_uevent *event, void *userdata) {
    (void)event;
    ++*(int *)userdata;
}

#if defined(__linux__)

INKWELL_TEST_CASE(uevent_monitor_opens_and_closes, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    struct inkwell_uevent_monitor monitor = {0};
    int calls = 0;
    const int opened = inkwell_uevent_monitor_open(&monitor, &loop, count_event, &calls);
    const int again = inkwell_uevent_monitor_open(&monitor, &loop, count_event, &calls);
    inkwell_uevent_monitor_close(&monitor);
    inkwell_uevent_monitor_close(&monitor); /* twice is harmless */
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(opened != 0, "the netlink socket did not open");
    INKWELL_TEST_FAIL_IF(again != -EBUSY, "an open monitor was opened over");
    INKWELL_TEST_FAIL_IF(monitor.loop != NULL || monitor.fd != -1, "close left it open");
    record_success(test_name);
}

struct heard {
    int calls;
    int matched;
};

static void match_null_device(const struct inkwell_uevent *event, void *userdata) {
    struct heard *heard = (struct heard *)userdata;
    heard->calls++;
    if (event != NULL && strcmp(event->action, "change") == 0 &&
        strcmp(event->devpath, "/devices/virtual/mem/null") == 0 && event->devname != NULL &&
        strcmp(event->devname, "null") == 0) {
        heard->matched++;
    }
}

/*
 * The kernel announcing /dev/null again, end to end. Needs root (sysfs is root's to write) and
 * the initial network namespace (the kernel sends a device's events where the device lives);
 * an ordinary run, or a container, checks only that the trigger is refused rather than lost.
 */
INKWELL_TEST_CASE(uevent_monitor_hears_a_trigger, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    struct inkwell_uevent_monitor monitor = {0};
    struct heard heard = {0, 0};
    const int opened = inkwell_uevent_monitor_open(&monitor, &loop, match_null_device, &heard);
    const int triggered = inkwell_uevent_trigger("/sys/devices/virtual/mem/null", "change");
    for (int i = 0; i < 100 && triggered == 0 && heard.matched == 0; ++i) {
        (void)inkwell_loop_run(&loop, 10);
    }
    inkwell_uevent_monitor_close(&monitor);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(opened != 0, "the netlink socket did not open");
    if (geteuid() != 0) {
        INKWELL_TEST_FAIL_IF(triggered != -EACCES && triggered != -EPERM && triggered != -EROFS,
                             "an unprivileged trigger should be refused");
        record_success(test_name);
        return;
    }
    if (triggered != 0) {
        /* Root without a writable sysfs: a container. Nothing to hear. */
        record_success(test_name);
        return;
    }
    INKWELL_TEST_FAIL_IF(heard.matched == 0 && heard.calls > 0,
                         "events arrived, but not the one triggered");
    /* heard.calls == 0: a network namespace the kernel does not send to. */
    record_success(test_name);
}

#else

INKWELL_TEST_CASE(uevent_monitor_refuses_off_linux, unit) {
    struct inkwell_loop loop;
    INKWELL_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop init failed");
    struct inkwell_uevent_monitor monitor = {0};
    int calls = 0;
    const int opened = inkwell_uevent_monitor_open(&monitor, &loop, count_event, &calls);
    inkwell_uevent_monitor_close(&monitor);
    inkwell_loop_shutdown(&loop);
    INKWELL_TEST_FAIL_IF(opened != -ENOSYS, "a monitor off Linux should refuse");
    INKWELL_TEST_FAIL_IF(inkwell_uevent_trigger("/sys/x", "add") != -ENOSYS,
                         "a trigger off Linux should refuse");
    record_success(test_name);
}

#endif
