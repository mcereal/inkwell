/*
 * The device mapper. dm-verity's line is checked everywhere against the one veritysetup prints for
 * the same tree; making a device needs root and the kernel's dm-mod, so elsewhere the calls must
 * refuse, and on Linux without root they must say so rather than half-make something.
 */
#include "framework/inkwell_test.h"

#include "inkwell/io/dm.h"

#include <errno.h>
#include <string.h>
#if defined(__linux__)
#include <unistd.h>
#endif

static const uint8_t k_root[32] = {
    0x5b, 0xf6, 0x2e, 0x5f, 0x0b, 0x8d, 0x41, 0x34, 0x6e, 0x1a, 0x52, 0x8e, 0x77, 0x07, 0x6b, 0x03,
    0xd0, 0x3e, 0x0a, 0x0e, 0x31, 0x4b, 0x9a, 0x50, 0x95, 0x2c, 0x8f, 0x37, 0x6d, 0x91, 0x10, 0xff,
};
static const uint8_t k_salt[4] = {0x00, 0x01, 0xa0, 0xff};

INKWELL_TEST_CASE(dm_verity_params_in_the_kernels_order, unit) {
    const struct inkwell_dm_verity v = {
        .data_device = "254:2",
        .hash_device = "254:2",
        .data_block_size = 4096U,
        .hash_block_size = 4096U,
        .data_blocks = 51200U,
        .hash_start_block = 51200U,
        .algorithm = "sha256",
        .root_digest = k_root,
        .root_digest_len = sizeof k_root,
        .salt = k_salt,
        .salt_len = sizeof k_salt,
    };
    char line[512];
    const int n = inkwell_dm_verity_params(&v, line, sizeof line);
    static const char expected[] =
        "1 254:2 254:2 4096 4096 51200 51200 sha256 "
        "5bf62e5f0b8d41346e1a528e77076b03d03e0a0e314b9a50952c8f376d9110ff 0001a0ff";
    INKWELL_TEST_FAIL_IF(n != (int)strlen(expected), "the length returned");
    INKWELL_TEST_FAIL_IF(strcmp(line, expected) != 0, "the line");

    struct inkwell_dm_verity with = v;
    with.options = "1 restart_on_corruption";
    with.salt_len = 0U;
    INKWELL_TEST_FAIL_IF(inkwell_dm_verity_params(&with, line, sizeof line) < 0, "with options");
    INKWELL_TEST_FAIL_IF(strstr(line, " - 1 restart_on_corruption") == NULL,
                         "no salt is '-', and options go last");
    INKWELL_TEST_FAIL_IF(inkwell_dm_verity_params(&v, line, 20U) != -ENOSPC, "too small a buffer");
    struct inkwell_dm_verity missing = v;
    missing.root_digest_len = 0U;
    INKWELL_TEST_FAIL_IF(inkwell_dm_verity_params(&missing, line, sizeof line) != -EINVAL,
                         "no root digest");
    record_success(test_name);
}

INKWELL_TEST_CASE(dm_create_refuses_without_the_kernel_or_rights, unit) {
    const struct inkwell_dm_target t = {.start = 0U, .sectors = 8U, .type = "error", .params = ""};
    struct inkwell_dm_device d;
#if defined(__linux__)
    if (geteuid() == 0) {
        /* A root run makes a real device out of the error target (dm-mod's own, so every kernel
         * with the device mapper has it), and removes it. */
        const int made = inkwell_dm_create("inkwell-test-dm", &t, 1U, true, &d);
        if (made == -ENOENT) {
            record_success(test_name); /* no /dev/mapper/control: a kernel without dm-mod */
            return;
        }
        INKWELL_TEST_FAIL_IF(made != 0, "an error device was not made");
        INKWELL_TEST_FAIL_IF(strncmp(d.path, "/dev/dm-", 8U) != 0, "its node");
        INKWELL_TEST_FAIL_IF(inkwell_dm_create("inkwell-test-dm", &t, 1U, true, NULL) != -EEXIST,
                             "a name taken");
        INKWELL_TEST_FAIL_IF(inkwell_dm_remove("inkwell-test-dm") != 0, "not removed");
        INKWELL_TEST_FAIL_IF(inkwell_dm_remove("inkwell-test-dm") != -ENXIO, "removed twice");
        record_success(test_name);
        return;
    }
    const int made = inkwell_dm_create("inkwell-test-dm", &t, 1U, true, &d);
    INKWELL_TEST_FAIL_IF(made == 0, "made a device without root");
#else
    INKWELL_TEST_FAIL_IF(inkwell_dm_create("inkwell-test-dm", &t, 1U, true, &d) != -ENOSYS,
                         "create off Linux");
    INKWELL_TEST_FAIL_IF(inkwell_dm_remove("inkwell-test-dm") != -ENOSYS, "remove off Linux");
#endif
    INKWELL_TEST_FAIL_IF(inkwell_dm_create("", &t, 1U, true, &d) != -EINVAL &&
                             inkwell_dm_create("", &t, 1U, true, &d) != -ENOSYS,
                         "an empty name");
    record_success(test_name);
}
