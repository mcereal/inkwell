#define _POSIX_C_SOURCE 200809L
#include "framework/inkwell_test.h"
#include "inkwell/base/file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

INKWELL_TEST_CASE(file_mkdir_makes_a_private_directory_once, unit) {
    char parent[] = "/tmp/inkwell_mkdir_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(parent) == NULL, "could not create a temporary directory");
    char path[sizeof parent + 16U];
    snprintf(path, sizeof path, "%s/child", parent);

    const int made = inkwell_file_mkdir(path);
    struct stat info;
    const bool is_dir = stat(path, &info) == 0 && S_ISDIR(info.st_mode);
    const unsigned mode = is_dir ? (unsigned)(info.st_mode & 0777) : 0U;
    /* The second call finds the first one's directory, and says so rather than succeeding. */
    const int again = inkwell_file_mkdir(path);

    rmdir(path);
    rmdir(parent);
    INKWELL_TEST_FAIL_IF(made != 0 || !is_dir, "the directory was not made");
    INKWELL_TEST_FAIL_IF(mode != 0700U, "the directory is readable by someone other than its owner");
    INKWELL_TEST_FAIL_IF(again != -EEXIST, "a directory already there was not reported as -EEXIST");
    record_success(test_name);
}

INKWELL_TEST_CASE(file_mkdir_makes_one_level_only, unit) {
    char parent[] = "/tmp/inkwell_mkdir_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(parent) == NULL, "could not create a temporary directory");
    char path[sizeof parent + 16U];
    snprintf(path, sizeof path, "%s/a/b", parent);

    const int made = inkwell_file_mkdir(path);
    rmdir(parent);
    INKWELL_TEST_FAIL_IF(made != -ENOENT, "a missing parent was made, or not reported as -ENOENT");
    INKWELL_TEST_FAIL_IF(inkwell_file_mkdir(NULL) != -EINVAL || inkwell_file_mkdir("") != -EINVAL,
                         "an empty path was not refused");
    record_success(test_name);
}
