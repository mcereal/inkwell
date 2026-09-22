#define _POSIX_C_SOURCE 200809L

#include "framework/inkwell_test.h"
#include "support/fs_fixture.h"

#include "inkwell/base/time.h"
#include "inkwell/io/usb_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct storage_fixture {
    char root[64];
    char block[128];
    char dev[128];
    char mounts[128];
};

static bool put_file(const char *path, const char *contents) {
    FILE *const file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    const bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool fixture_open(struct storage_fixture *fixture) {
    char root[] = "/tmp/inkwell-storage-XXXXXX";
    if (mkdtemp(root) == NULL) {
        return false;
    }
    snprintf(fixture->root, sizeof fixture->root, "%s", root);
    snprintf(fixture->block, sizeof fixture->block, "%s/block", root);
    snprintf(fixture->dev, sizeof fixture->dev, "%s/dev", root);
    snprintf(fixture->mounts, sizeof fixture->mounts, "%s/mounts", root);
    return mkdir(fixture->block, 0700) == 0 && mkdir(fixture->dev, 0700) == 0 &&
           put_file(fixture->mounts, "") && setenv("INKWELL_SYSFS_BLOCK", fixture->block, 1) == 0 &&
           setenv("INKWELL_DEV_ROOT", fixture->dev, 1) == 0 &&
           setenv("INKWELL_PROC_MOUNTS", fixture->mounts, 1) == 0;
}

static void fixture_close(struct storage_fixture *fixture) {
    (void)unsetenv("INKWELL_SYSFS_BLOCK");
    (void)unsetenv("INKWELL_DEV_ROOT");
    (void)unsetenv("INKWELL_PROC_MOUNTS");
    (void)inkwell_test_remove_tree(fixture->root);
}

static bool add_device(struct storage_fixture *fixture, const char *name, const char *usb_id) {
    char path[PATH_MAX];
    char target[PATH_MAX];
    char size[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", fixture->block, name) >= (int)sizeof path ||
        snprintf(target, sizeof target, "usb2/%s/%s:1.2/block/%s", usb_id, usb_id, name) >=
            (int)sizeof target ||
        snprintf(size, sizeof size, "%s/%s", fixture->dev, name) >= (int)sizeof size) {
        return false;
    }
    return symlink(target, path) == 0 && put_file(size, "");
}

INKWELL_TEST_CASE(usb_storage_finds_sibling_drive_and_mounts, unit) {
    struct storage_fixture fixture;
    INKWELL_TEST_FAIL_IF(!fixture_open(&fixture), "could not create storage fixture");
    bool built = add_device(&fixture, "sda", "2-1") && add_device(&fixture, "sdb", "2-1.4");
    char mounts[1024];
    snprintf(mounts, sizeof mounts,
             "%s/sda1 /mnt/first vfat rw 0 0\n"
             "%s/sdb /mnt/other vfat rw 0 0\n"
             "%s/sda /mnt/second\\040mount vfat rw 0 0\n",
             fixture.dev, fixture.dev, fixture.dev);
    built = built && put_file(fixture.mounts, mounts);
    INKWELL_TEST_FAIL_IF_CLEANUP(!built, fixture_close(&fixture), "could not build drives");

    struct inkwell_serial_port_info port = {0};
    snprintf(port.id, sizeof port.id, "%s", "2-1:1.1");
    struct inkwell_usb_storage_target target;
    const int found = inkwell_usb_storage_find(&port, &target);
    char expected[128];
    snprintf(expected, sizeof expected, "%s/sda", fixture.dev);
    const bool right = found == 0 && strcmp(target.device, expected) == 0 &&
                       target.mount_count == 2U && strcmp(target.mounts[0], "/mnt/first") == 0 &&
                       strcmp(target.mounts[1], "/mnt/second mount") == 0;

    snprintf(port.id, sizeof port.id, "%s", "2-1.4:1.1");
    struct inkwell_usb_storage_target sibling;
    const bool separate = inkwell_usb_storage_find(&port, &sibling) == 0 &&
                          strstr(sibling.device, "/sdb") != NULL && sibling.mount_count == 1U;
    fixture_close(&fixture);
    INKWELL_TEST_FAIL_IF(!right, "the sibling interface should find its drive and mounts");
    INKWELL_TEST_FAIL_IF(!separate, "a USB device with a shared prefix has its own drive");
    record_success(test_name);
}

INKWELL_TEST_CASE(usb_storage_refuses_partial_unmount, unit) {
    struct inkwell_usb_storage_target target = {0};
    target.too_many_mounts = true;
    target.mount_count = INKWELL_USB_STORAGE_MOUNTS_MAX;
    INKWELL_TEST_FAIL_IF(inkwell_usb_storage_unmount(&target) != -E2BIG,
                         "overflow must refuse before touching any mount");
    INKWELL_TEST_FAIL_IF(target.mount_count != INKWELL_USB_STORAGE_MOUNTS_MAX,
                         "a refused unmount must retain the mount list");
    record_success(test_name);
}

INKWELL_TEST_CASE(usb_storage_claim_creates_nothing, unit) {
    struct storage_fixture fixture;
    INKWELL_TEST_FAIL_IF(!fixture_open(&fixture), "could not create storage fixture");
    char path[128];
    snprintf(path, sizeof path, "%s/drive", fixture.dev);
    const int missing = inkwell_usb_storage_claim(path);
    const bool absent = access(path, F_OK) != 0;
    const bool seeded = put_file(path, "");
    const int claimed = seeded ? inkwell_usb_storage_claim(path) : -1;
    if (claimed >= 0) {
        close(claimed);
    }
    fixture_close(&fixture);
    INKWELL_TEST_FAIL_IF(missing != -ENOENT || !absent, "claim must not create a vanished drive");
    INKWELL_TEST_FAIL_IF(claimed < 0, "an existing file should exercise the claim path");
    record_success(test_name);
}

INKWELL_TEST_CASE(usb_storage_writer_syncs_and_reports_bytes, unit) {
    struct storage_fixture fixture;
    INKWELL_TEST_FAIL_IF(!fixture_open(&fixture), "could not create storage fixture");
    char path[128];
    snprintf(path, sizeof path, "%s/drive", fixture.dev);
    INKWELL_TEST_FAIL_IF_CLEANUP(!put_file(path, ""), fixture_close(&fixture),
                                 "could not seed drive");
    uint8_t image[65537];
    for (size_t i = 0; i < sizeof image; ++i) {
        image[i] = (uint8_t)i;
    }
    struct inkwell_usb_storage_write writer;
    inkwell_usb_storage_write_init(&writer);
    const int started = inkwell_usb_storage_write_start(&writer, NULL, image, sizeof image, path,
                                                        inkwell_time_monotonic_ms());
    INKWELL_TEST_FAIL_IF_CLEANUP(started != 0, fixture_close(&fixture), "write should start");
    const uint64_t deadline = inkwell_time_monotonic_ms() + 3000U;
    while (writer.state == INKWELL_USB_STORAGE_WRITE_RUNNING &&
           inkwell_time_monotonic_ms() < deadline) {
        inkwell_usb_storage_write_tick(&writer, inkwell_time_monotonic_ms());
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    const bool done = writer.state == INKWELL_USB_STORAGE_WRITE_DONE &&
                      writer.written == sizeof image &&
                      inkwell_usb_storage_write_progress(&writer) == 100U;
    FILE *const file = fopen(path, "rb");
    uint8_t actual[sizeof image];
    const bool equal = file != NULL && fread(actual, 1U, sizeof actual, file) == sizeof actual &&
                       memcmp(image, actual, sizeof image) == 0;
    if (file != NULL) {
        fclose(file);
    }
    inkwell_usb_storage_write_cancel(&writer);
    fixture_close(&fixture);
    INKWELL_TEST_FAIL_IF(!done, "write should finish with acknowledged progress");
    INKWELL_TEST_FAIL_IF(!equal, "every byte should land on the device");
    record_success(test_name);
}

INKWELL_TEST_CASE(usb_storage_writer_releases_claim_on_fd_zero, unit) {
    struct storage_fixture fixture;
    INKWELL_TEST_FAIL_IF(!fixture_open(&fixture), "could not create storage fixture");
    char path[128];
    snprintf(path, sizeof path, "%s/drive", fixture.dev);
    INKWELL_TEST_FAIL_IF_CLEANUP(!put_file(path, ""), fixture_close(&fixture),
                                 "could not seed drive");

    const int saved_stdin = dup(STDIN_FILENO);
    INKWELL_TEST_FAIL_IF_CLEANUP(saved_stdin < 0, fixture_close(&fixture), "could not save stdin");
    const uint8_t image[] = {1U, 2U, 3U, 4U};
    struct inkwell_usb_storage_write writer;
    inkwell_usb_storage_write_init(&writer);
    close(STDIN_FILENO);
    const int started = inkwell_usb_storage_write_start(&writer, NULL, image, sizeof image, path,
                                                        inkwell_time_monotonic_ms());
    const bool claimed_zero = started == 0 && writer.device_fd == STDIN_FILENO;
    const uint64_t deadline = inkwell_time_monotonic_ms() + 3000U;
    while (writer.state == INKWELL_USB_STORAGE_WRITE_RUNNING &&
           inkwell_time_monotonic_ms() < deadline) {
        inkwell_usb_storage_write_tick(&writer, inkwell_time_monotonic_ms());
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    const bool done = writer.state == INKWELL_USB_STORAGE_WRITE_DONE;
    const bool released =
        writer.device_fd == -1 && fcntl(STDIN_FILENO, F_GETFD) == -1 && errno == EBADF;
    inkwell_usb_storage_write_cancel(&writer);
    const int restored = dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    fixture_close(&fixture);
    INKWELL_TEST_FAIL_IF(restored < 0, "could not restore stdin");
    INKWELL_TEST_FAIL_IF(!claimed_zero, "the device claim should use descriptor zero");
    INKWELL_TEST_FAIL_IF(!done || !released, "completion must release descriptor zero");
    record_success(test_name);
}
