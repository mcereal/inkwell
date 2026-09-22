#define _POSIX_C_SOURCE 200809L
#include "framework/inkwell_test.h"
#include "inkwell/base/record_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct record_fixture {
    unsigned seen;
    char value[80];
};

static void write_snapshot(FILE *file, void *context) {
    (void)context;
    fputs("# comment\n", file);
    fputs("name=", file);
    inkwell_record_write_escaped(file, "a=b\\c\nend");
    fputc('\n', file);
}

static void write_tail(FILE *file, void *context) {
    (void)context;
    fputs("tail=2\n", file);
}

static void visit_record(void *context, const char *key, char *value) {
    struct record_fixture *fixture = context;
    fixture->seen++;
    if (strcmp(key, "name") == 0) {
        snprintf(fixture->value, sizeof fixture->value, "%s", value);
    }
}

INKWELL_TEST_CASE(record_file_round_trip, unit) {
    char path[] = "/tmp/inkwell_record_XXXXXX";
    int fd = mkstemp(path);
    INKWELL_TEST_FAIL_IF(fd < 0, "could not create temporary file");
    close(fd);
    char temp[sizeof path + 8U];
    int result = inkwell_record_replace(path, temp, sizeof temp, write_snapshot, NULL, true);
    if (result == 0) {
        result = inkwell_record_append(path, write_tail, NULL);
    }
    FILE *file = fopen(path, "r");
    struct record_fixture fixture = {0};
    char line[128];
    if (result == 0 && file != NULL) {
        result = inkwell_record_read(file, line, sizeof line, visit_record, &fixture);
    }
    if (file != NULL) {
        fclose(file);
    }
    unlink(path);
    INKWELL_TEST_FAIL_IF(result != 0 || fixture.seen != 2U ||
                             strcmp(fixture.value, "a=b\\c\nend") != 0,
                         "replacement, append or escape round trip failed");
    record_success(test_name);
}

INKWELL_TEST_CASE(record_file_discards_long_line, unit) {
    FILE *file = tmpfile();
    INKWELL_TEST_FAIL_IF(file == NULL, "could not create scratch file");
    fputs("01234567890123456789=forged\nkeep=ok\n", file);
    rewind(file);
    char line[16];
    struct record_fixture fixture = {0};
    const int result = inkwell_record_read(file, line, sizeof line, visit_record, &fixture);
    fclose(file);
    INKWELL_TEST_FAIL_IF(result != 0 || fixture.seen != 1U, "a truncated line became a record");
    record_success(test_name);
}
