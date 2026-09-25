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
    INKWELL_TEST_FAIL_IF(mode != 0700U,
                         "the directory is readable by someone other than its owner");
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

static bool file_holds(const char *path, const char *expected) {
    char buffer[32] = {0};
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    const size_t got = fread(buffer, 1U, sizeof buffer - 1U, file);
    fclose(file);
    return got == strlen(expected) && memcmp(buffer, expected, got) == 0;
}

static bool file_write(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    fputs(text, file);
    return fclose(file) == 0;
}

INKWELL_TEST_CASE(file_replace_moves_over_an_existing_file, unit) {
    char parent[] = "/tmp/inkwell_replace_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(parent) == NULL, "could not create a temporary directory");
    char from[sizeof parent + 16U];
    char to[sizeof parent + 16U];
    snprintf(from, sizeof from, "%s/new", parent);
    snprintf(to, sizeof to, "%s/old", parent);

    const bool staged = file_write(to, "old") && file_write(from, "new");
    const int replaced = inkwell_file_replace(from, to);
    const bool moved = file_holds(to, "new") && access(from, F_OK) != 0;
    const int missing = inkwell_file_replace(from, to);

    remove(to);
    remove(from);
    rmdir(parent);
    INKWELL_TEST_FAIL_IF(!staged, "could not stage the two files");
    INKWELL_TEST_FAIL_IF(replaced != 0 || !moved, "the file was not moved over the old one");
    INKWELL_TEST_FAIL_IF(missing != -ENOENT, "a missing source was not reported as -ENOENT");
    INKWELL_TEST_FAIL_IF(inkwell_file_replace(NULL, to) != -EINVAL ||
                             inkwell_file_replace(from, "") != -EINVAL,
                         "an empty path was not refused");
    record_success(test_name);
}
