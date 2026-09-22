#define _XOPEN_SOURCE 700

/*
 * USB serial ports: the scan's reading of a USB tree, the mock that stands in for all of it, and
 * the one real thing a test can open - a pseudo-terminal - to hold the termios settings.
 *
 * Nothing here reaches real sysfs, usbfs or a device: the scan is pointed at a tree laid out in
 * a temporary directory, which on macOS replaces the I/O Registry too.
 */

#include "framework/inkwell_test.h"
#include "support/fs_fixture.h"

#include "inkwell/io/serial.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static bool fixture_write(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    const bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool fixture_dir(const char *root, const char *name) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir) {
        return false;
    }
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

/* One interface directory: <root>/<name>/{bInterfaceClass,SubClass,Protocol,Number}, plus a
   driver symlink when a driver has claimed it. */
static bool fixture_interface(const char *root, const char *name, const char *cls,
                              const char *subclass, const char *protocol, const char *number,
                              const char *driver) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir ||
        !fixture_dir(root, name)) {
        return false;
    }
    struct {
        const char *attr;
        const char *value;
    } attrs[] = {{"bInterfaceClass", cls},
                 {"bInterfaceSubClass", subclass},
                 {"bInterfaceProtocol", protocol},
                 {"bInterfaceNumber", number}};
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i].attr) >= (int)sizeof file ||
            !fixture_write(file, attrs[i].value)) {
            return false;
        }
    }
    if (driver != NULL) {
        char parent[PATH_MAX];
        char target[PATH_MAX];
        if (snprintf(parent, sizeof parent, "%s/drivers", root) >= (int)sizeof parent ||
            snprintf(target, sizeof target, "%s/%s", parent, driver) >= (int)sizeof target ||
            snprintf(file, sizeof file, "%s/driver", dir) >= (int)sizeof file) {
            return false;
        }
        if ((mkdir(parent, 0755) != 0 && errno != EEXIST) ||
            (mkdir(target, 0755) != 0 && errno != EEXIST)) {
            return false;
        }
        if (symlink(target, file) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

/* The device directory the interfaces hang off: <root>/<name>/{idVendor,idProduct,...}. */
static bool fixture_device(const char *root, const char *name, const char *vid, const char *pid,
                           const char *product) {
    char dir[PATH_MAX];
    char file[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, name) >= (int)sizeof dir ||
        !fixture_dir(root, name)) {
        return false;
    }
    struct {
        const char *attr;
        const char *value;
    } attrs[] = {
        {"idVendor", vid}, {"idProduct", pid}, {"product", product},
        {"busnum", "2"},   {"devnum", "3"},
    };
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        if (snprintf(file, sizeof file, "%s/%s", dir, attrs[i].attr) >= (int)sizeof file ||
            !fixture_write(file, attrs[i].value)) {
            return false;
        }
    }
    return true;
}

static const struct inkwell_serial_port_info *
find_by_id(const struct inkwell_serial_port_info *list, size_t count, const char *id) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i].id, id) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

/*
 * A tree laid out as the Brick's sysfs was measured on 2026-09-10, with a T114 and a Heltec V3.
 * The mock replaces the scan whole, so it can prove nothing about how a port's kind is read;
 * this is the test that does.
 */
INKWELL_TEST_CASE(serial_scan_reads_the_usb_tree, unit) {
    char root[] = "/tmp/inkwell-sysfs-XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(root) == NULL, "could not make a fixture sysfs tree");

    bool built = true;
    /* 2-1: an nRF52840 running firmware - its own USB, a CDC pair, no driver, no drive. */
    built = built && fixture_device(root, "2-1", "239a", "4405", "HT-n5262");
    built = built && fixture_interface(root, "2-1:1.0", "02", "02", "00", "00", NULL);
    built = built && fixture_interface(root, "2-1:1.1", "0a", "00", "00", "01", NULL);
    /* 3-1: the same board in its UF2 bootloader - a mass-storage Bulk-Only interface beside
       the CDC pair. */
    built = built && fixture_device(root, "3-1", "239a", "0071", "HT-n5262");
    built = built && fixture_interface(root, "3-1:1.0", "02", "02", "00", "00", NULL);
    built = built && fixture_interface(root, "3-1:1.1", "0a", "00", "00", "01", NULL);
    built = built && fixture_interface(root, "3-1:1.2", "08", "06", "50", "02", "usb-storage");
    /* 4-1: a CP2102 bridge - one vendor-class interface, its driver bound, its tty published. */
    built = built && fixture_device(root, "4-1", "10c4", "ea60", "CP2102 USB to UART Bridge");
    built = built && fixture_interface(root, "4-1:1.0", "ff", "00", "00", "00", "cp210x");
    built = built && fixture_dir(root, "4-1:1.0/ttyUSB0");
    /* 5-1: a hub's interface - neither CDC-Data nor on a serial driver. Not a port. */
    built = built && fixture_device(root, "5-1", "05e3", "0610", "USB2.0 Hub");
    built = built && fixture_interface(root, "5-1:1.0", "09", "00", "00", "00", "hub");
    INKWELL_TEST_FAIL_IF_CLEANUP(!built, (void)inkwell_test_remove_tree(root),
                                 "could not lay out the fixture tree");

    inkwell_serial_set_sysfs_root(root);
    struct inkwell_serial_port_info ports[8];
    const size_t count = inkwell_serial_scan(ports, 8U);
    const size_t capped = inkwell_serial_scan(ports, 1U);
    inkwell_serial_set_sysfs_root(NULL);
    (void)inkwell_test_remove_tree(root);

    INKWELL_TEST_FAIL_IF(capped != 1U, "the scan should stop at the capacity it was given");
    INKWELL_TEST_FAIL_IF(count != 3U, "the scan should offer the three serial interfaces");

    const struct inkwell_serial_port_info *native = find_by_id(ports, count, "2-1:1.1");
    const struct inkwell_serial_port_info *boot = find_by_id(ports, count, "3-1:1.1");
    const struct inkwell_serial_port_info *bridge = find_by_id(ports, count, "4-1:1.0");
    INKWELL_TEST_FAIL_IF(native == NULL || boot == NULL || bridge == NULL,
                         "the scan lost one of the three ports");

    INKWELL_TEST_FAIL_IF(native->kind != INKWELL_SERIAL_NATIVE || native->mass_storage,
                         "a CDC pair with no drive is a native port and nothing more");
    INKWELL_TEST_FAIL_IF(boot->kind != INKWELL_SERIAL_NATIVE || !boot->mass_storage,
                         "a CDC pair with a Bulk-Only drive beside it should say so");
    INKWELL_TEST_FAIL_IF(bridge->kind != INKWELL_SERIAL_BRIDGE || bridge->mass_storage,
                         "a vendor-class interface on a serial driver is a UART bridge");

    INKWELL_TEST_FAIL_IF(native->bound || native->path[0] != '\0' || !native->needs_line_state,
                         "an unbound native port has no tty yet and needs the usbfs line state");
    INKWELL_TEST_FAIL_IF(native->control_interface != 0 || boot->control_interface != 0,
                         "the CDC control interface should be found at 0");
    INKWELL_TEST_FAIL_IF(!bridge->bound || strcmp(bridge->path, "/dev/ttyUSB0") != 0,
                         "a bridge's published tty should be its path");
    INKWELL_TEST_FAIL_IF(bridge->needs_line_state || bridge->control_interface >= 0,
                         "a bridge has no CDC control interface to poke");

    INKWELL_TEST_FAIL_IF(native->vendor_id != 0x239AU || native->product_id != 0x4405U ||
                             native->busnum != 2U || native->devnum != 3U,
                         "the device's numbers should come off its own directory");
    INKWELL_TEST_FAIL_IF(strcmp(bridge->name, "CP2102 USB to UART Bridge") != 0,
                         "the name should be the USB product string");
}

INKWELL_TEST_CASE(serial_scan_without_a_tree_finds_nothing, unit) {
    inkwell_serial_set_sysfs_root("/nonexistent/inkwell-sysfs");
    struct inkwell_serial_port_info ports[2];
    const size_t count = inkwell_serial_scan(ports, 2U);
    inkwell_serial_set_sysfs_root(NULL);
    INKWELL_TEST_FAIL_IF(count != 0U, "a missing tree should be no ports, not an error");
}

INKWELL_TEST_CASE(serial_mock_stands_in_for_the_system, unit) {
    struct inkwell_serial_port_info port;
    memset(&port, 0, sizeof port);
    snprintf(port.id, sizeof port.id, "%s", "1-1:1.1");
    port.kind = INKWELL_SERIAL_NATIVE;
    port.control_interface = 0;

    int pair[2] = {-1, -1};
    INKWELL_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "no socketpair");

    struct inkwell_serial_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.ports = &port;
    mock.port_count = 1U;
    mock.bound_path = "/dev/ttyUSB7";
    mock.open_fd = pair[1];
    inkwell_serial_mock_enable(&mock);

    struct inkwell_serial_port_info found[4];
    memset(found, 0, sizeof found);
    const size_t count = inkwell_serial_scan(found, 4U);
    const int bound = count == 1U ? inkwell_serial_bind(&found[0]) : -1;
    const int line = inkwell_serial_set_line_state(&found[0], true, true);
    const int fd = inkwell_serial_open(found[0].path, 115200U);
    const size_t binds = inkwell_serial_mock_bind_calls();
    const size_t lines = inkwell_serial_mock_line_state_calls();

    const char hello[] = "hi";
    const bool carried = fd >= 0 && write(fd, hello, 2) == 2;
    char got[2] = {0};
    const bool arrived = carried && read(pair[0], got, sizeof got) == 2 && memcmp(got, "hi", 2) == 0;
    inkwell_serial_close(fd);

    mock.open_result = -EACCES;
    inkwell_serial_mock_enable(&mock);
    const int refused = inkwell_serial_open("/dev/ttyUSB7", 115200U);
    inkwell_serial_mock_disable();
    close(pair[0]);
    close(pair[1]);

    INKWELL_TEST_FAIL_IF(count != 1U || strcmp(found[0].id, "1-1:1.1") != 0,
                         "the scan should report the scripted port");
    INKWELL_TEST_FAIL_IF(bound != 0 || !found[0].bound || strcmp(found[0].path, "/dev/ttyUSB7") != 0,
                         "a bind should publish the scripted path");
    INKWELL_TEST_FAIL_IF(line != 0, "the line state should answer the scripted result");
    INKWELL_TEST_FAIL_IF(binds != 1U || lines != 1U, "each call should be counted once");
    INKWELL_TEST_FAIL_IF(!arrived, "an open should hand back the scripted descriptor");
    INKWELL_TEST_FAIL_IF(refused != -EACCES, "a scripted open failure should be returned");
}

/* The one real device a test can open: a pseudo-terminal holds termios exactly as a tty does. */
INKWELL_TEST_CASE(serial_open_makes_the_tty_raw, unit) {
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    INKWELL_TEST_FAIL_IF(master < 0, "no pseudo-terminal");
    INKWELL_TEST_FAIL_IF_CLEANUP(grantpt(master) != 0 || unlockpt(master) != 0, close(master),
                                 "could not unlock the pseudo-terminal");
    char path[64];
    snprintf(path, sizeof path, "%s", ptsname(master));

    const int wrong = inkwell_serial_open(path, 12345U);
    const int fd = inkwell_serial_open(path, 115200U);
    struct termios tio;
    memset(&tio, 0, sizeof tio);
    const bool read_back = fd >= 0 && tcgetattr(fd, &tio) == 0;
    const int flags = fd >= 0 ? fcntl(fd, F_GETFL) : 0;
    inkwell_serial_close(fd);
    close(master);

    INKWELL_TEST_FAIL_IF(wrong != -EINVAL, "a rate termios has no constant for is refused");
    INKWELL_TEST_FAIL_IF(!read_back, "the tty should open at 115200");
    INKWELL_TEST_FAIL_IF((flags & O_NONBLOCK) == 0, "the tty should be non-blocking");
    INKWELL_TEST_FAIL_IF(cfgetospeed(&tio) != B115200, "the rate should be the one asked for");
    INKWELL_TEST_FAIL_IF((tio.c_lflag & (ICANON | ECHO)) != 0, "the tty should be raw");
    INKWELL_TEST_FAIL_IF(tio.c_cc[VMIN] != 1 || tio.c_cc[VTIME] != 0,
                         "VMIN 1 is what makes an empty buffer EAGAIN rather than EOF");
    INKWELL_TEST_FAIL_IF(inkwell_serial_open("", 115200U) != -EINVAL, "an empty path is refused");
}
