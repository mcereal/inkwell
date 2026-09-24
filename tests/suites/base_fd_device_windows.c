#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <string.h>

struct fake_device {
    int reads;
    int writes;
    int closes;
};

static int fake_read(void *context, void *bytes, size_t len) {
    struct fake_device *device = (struct fake_device *)context;
    device->reads += 1;
    memset(bytes, 0x5A, len);
    return (int)len;
}

static int fake_write(void *context, const void *bytes, size_t len) {
    (void)bytes;
    struct fake_device *device = (struct fake_device *)context;
    device->writes += 1;
    return len > 0U ? -EAGAIN : 0;
}

static int fake_close(void *context) {
    struct fake_device *device = (struct fake_device *)context;
    device->closes += 1;
    return 0;
}

static const struct inkwell_fd_device_ops kFakeOps = {
    .read = fake_read,
    .write = fake_write,
    .close = fake_close,
};

/* A number nothing in the CRT will hand out, like the loop's handle tokens. */
#define FAKE_DEVICE_FD 0x40000123

INKWELL_TEST_CASE(fd_device_answers_for_its_number_until_closed, unit) {
    struct fake_device device = {0};
    INKWELL_TEST_FAIL_IF(inkwell_fd_attach_device(FAKE_DEVICE_FD, &kFakeOps, &device) != 0,
                         "a device should attach");
    const int again = inkwell_fd_attach_device(FAKE_DEVICE_FD, &kFakeOps, &device);
    uint8_t bytes[4] = {0};
    const int read = inkwell_fd_read(FAKE_DEVICE_FD, bytes, sizeof bytes);
    const int written = inkwell_fd_write(FAKE_DEVICE_FD, bytes, sizeof bytes);
    const int duplicate = inkwell_fd_dup(FAKE_DEVICE_FD);
    const int closed = inkwell_fd_close(FAKE_DEVICE_FD);
    /* Detached: the number is the CRT's again, and the CRT has never heard of it. */
    const int after = inkwell_fd_read(FAKE_DEVICE_FD, bytes, sizeof bytes);
    INKWELL_TEST_FAIL_IF(again != -EEXIST, "one number is one device");
    INKWELL_TEST_FAIL_IF(read != 4 || bytes[0] != 0x5A || device.reads != 1,
                         "a read should reach the device");
    INKWELL_TEST_FAIL_IF(written != -EAGAIN || device.writes != 1,
                         "a write should reach the device and keep its answer");
    INKWELL_TEST_FAIL_IF(duplicate != -ENOTSUP, "a device cannot be duplicated");
    INKWELL_TEST_FAIL_IF(closed != 0 || device.closes != 1, "close should reach the device once");
    INKWELL_TEST_FAIL_IF(after >= 0 || device.reads != 1, "a closed device hears nothing more");
    record_success(test_name);
}

INKWELL_TEST_CASE(fd_device_rejects_an_incomplete_attach, unit) {
    const struct inkwell_fd_device_ops partial = {.read = fake_read, .write = fake_write};
    INKWELL_TEST_FAIL_IF(inkwell_fd_attach_device(-1, &kFakeOps, NULL) != -EINVAL,
                         "a negative number is not a device");
    INKWELL_TEST_FAIL_IF(inkwell_fd_attach_device(FAKE_DEVICE_FD, NULL, NULL) != -EINVAL,
                         "a device needs ops");
    INKWELL_TEST_FAIL_IF(inkwell_fd_attach_device(FAKE_DEVICE_FD, &partial, NULL) != -EINVAL,
                         "a device needs all three ops");
    record_success(test_name);
}
