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

INKWELL_TEST_CASE(file_is_dir_tells_a_directory_from_a_file, unit) {
    char parent[] = "/tmp/inkwell_isdir_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(parent) == NULL, "could not create a temporary directory");
    char file[sizeof parent + 16U];
    snprintf(file, sizeof file, "%s/file", parent);
    const bool staged = file_write(file, "x");

    const bool dir_is = inkwell_file_is_dir(parent);
    const bool file_is = inkwell_file_is_dir(file);
    /* The case the helper is for: mkdir over a file reports -EEXIST, and this is what says the
       thing that exists is not a directory. */
    const int made = inkwell_file_mkdir(file);

    remove(file);
    rmdir(parent);
    INKWELL_TEST_FAIL_IF(!staged, "could not stage the file");
    INKWELL_TEST_FAIL_IF(!dir_is, "a directory was not reported as one");
    INKWELL_TEST_FAIL_IF(file_is || made != -EEXIST, "a file was reported as a directory");
    INKWELL_TEST_FAIL_IF(inkwell_file_is_dir(NULL) || inkwell_file_is_dir("") ||
                             inkwell_file_is_dir("/nonexistent/inkwell/path"),
                         "nothing at all was reported as a directory");
    record_success(test_name);
}

struct list_probe {
    const char *dir;
    unsigned seen;
    bool saw_dots;
    unsigned removed;
};

static void list_visit(void *context, const char *name) {
    struct list_probe *probe = context;
    probe->seen++;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        probe->saw_dots = true;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/%s", probe->dir, name);
    if (remove(path) == 0) {
        probe->removed++;
    }
}

INKWELL_TEST_CASE(file_list_visits_every_entry_and_lets_the_visitor_remove_it, unit) {
    char parent[] = "/tmp/inkwell_list_XXXXXX";
    INKWELL_TEST_FAIL_IF(mkdtemp(parent) == NULL, "could not create a temporary directory");
    char path[sizeof parent + 16U];
    bool staged = true;
    for (int i = 0; i < 5; ++i) {
        snprintf(path, sizeof path, "%s/f%d", parent, i);
        staged = staged && file_write(path, "x");
    }

    struct list_probe probe = {parent, 0U, false, 0U};
    const int listed = inkwell_file_list(parent, list_visit, &probe);
    snprintf(path, sizeof path, "%s/f0", parent);
    const bool is_file = file_write(path, "x");
    struct list_probe unused = {parent, 0U, false, 0U};
    const int not_dir = inkwell_file_list(path, list_visit, &unused);
    remove(path);
    const int empty = rmdir(parent);

    INKWELL_TEST_FAIL_IF(!staged || !is_file, "could not stage the files");
    INKWELL_TEST_FAIL_IF(listed != 0 || probe.seen != 5U || probe.removed != 5U,
                         "an entry was missed, or removing one stopped the walk");
    INKWELL_TEST_FAIL_IF(probe.saw_dots, ". or .. was handed to the visitor");
    INKWELL_TEST_FAIL_IF(empty != 0, "the directory was not left empty");
    INKWELL_TEST_FAIL_IF(not_dir != -ENOTDIR || unused.seen != 0U,
                         "a file was listed as a directory");
    INKWELL_TEST_FAIL_IF(inkwell_file_list("", list_visit, &unused) != -EINVAL ||
                             inkwell_file_list(parent, NULL, NULL) != -EINVAL,
                         "an empty path or no visitor was not refused");
    record_success(test_name);
}
