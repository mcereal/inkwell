/*
 * Partitions by name. The uevent text parser is tested everywhere; the lookup runs against a fake
 * /sys laid out in a temporary directory, as the kernel writes /sys/class/block: one entry per
 * disk and per partition, each with a uevent file and a size in sectors.
 */
#include "framework/inkwell_test.h"
#include "support/fs_fixture.h"

#include "inkwell/io/block.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char k_partition[] = "MAJOR=254\nMINOR=2\nDEVNAME=vda2\nDEVTYPE=partition\n"
                                  "DISKSEQ=9\nPARTN=2\nPARTNAME=system_a\n";

INKWELL_TEST_CASE(block_uevent_value_reads_a_key, unit) {
    char value[32];
    INKWELL_TEST_FAIL_IF(inkwell_block_uevent_value(k_partition, "PARTNAME", value, sizeof value),
                         "PARTNAME not found");
    INKWELL_TEST_FAIL_IF(strcmp(value, "system_a") != 0, "PARTNAME's value");
    INKWELL_TEST_FAIL_IF(inkwell_block_uevent_value(k_partition, "MAJOR", value, sizeof value),
                         "MAJOR not found");
    INKWELL_TEST_FAIL_IF(strcmp(value, "254") != 0, "MAJOR's value");
    /* A key that is a prefix of another is not that other. */
    INKWELL_TEST_FAIL_IF(inkwell_block_uevent_value(k_partition, "PART", value, sizeof value) !=
                             -ENOENT,
                         "a prefix matched");
    INKWELL_TEST_FAIL_IF(inkwell_block_uevent_value(k_partition, "PARTNAME", value, 4U) != -ENOSPC,
                         "a value too long for the buffer");
    /* The last line may have no newline. */
    INKWELL_TEST_FAIL_IF(inkwell_block_uevent_value("A=1\nB=2", "B", value, sizeof value) != 0 ||
                             strcmp(value, "2") != 0,
                         "a last line without a newline");
    record_success(test_name);
}

#if !defined(_WIN32)
static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fputs(text, f);
    return fclose(f);
}

/* class/block/<entry> with its uevent and size. */
static int add_entry(const char *sys, const char *entry, const char *uevent, const char *size) {
    char path[512];
    snprintf(path, sizeof path, "%s/class/block/%s", sys, entry);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, sizeof path, "%s/class/block/%s/uevent", sys, entry);
    if (write_file(path, uevent) != 0) {
        return -1;
    }
    snprintf(path, sizeof path, "%s/class/block/%s/size", sys, entry);
    return write_file(path, size);
}

static int make_sys(char *dir) {
    char path[512];
    if (mkdtemp(dir) == NULL) {
        return -1;
    }
    snprintf(path, 512, "%s/class", dir);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    snprintf(path, 512, "%s/class/block", dir);
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    /* The disk itself has no PARTNAME; neither has a partition of an MBR table. */
    return add_entry(dir, "vda", "MAJOR=254\nMINOR=0\nDEVNAME=vda\nDEVTYPE=disk\n", "4194304\n") ||
           add_entry(dir, "vda1",
                     "MAJOR=254\nMINOR=1\nDEVNAME=vda1\nDEVTYPE=partition\n"
                     "PARTN=1\nPARTNAME=misc\n",
                     "2048\n") ||
           add_entry(dir, "vda2", k_partition, "1048576\n") ||
           add_entry(dir, "loop0", "MAJOR=7\nMINOR=0\nDEVNAME=loop0\nDEVTYPE=disk\n", "0\n");
}

INKWELL_TEST_CASE(block_find_names_a_partition, unit) {
    char dir[] = "/tmp/inkwell-block-XXXXXX";
    INKWELL_TEST_FAIL_IF_CLEANUP(make_sys(dir) != 0, inkwell_test_remove_tree(dir),
                                 "could not lay out a fake /sys");
    struct inkwell_block_partition p;
    const int found = inkwell_block_find(dir, "system_a", &p);
    INKWELL_TEST_FAIL_IF_CLEANUP(found != 0, inkwell_test_remove_tree(dir), "system_a not found");
    INKWELL_TEST_FAIL_IF_CLEANUP(strcmp(p.path, "/dev/vda2") != 0, inkwell_test_remove_tree(dir),
                                 "the node's path");
    INKWELL_TEST_FAIL_IF_CLEANUP(p.major != 254U || p.minor != 2U, inkwell_test_remove_tree(dir),
                                 "the device number");
    INKWELL_TEST_FAIL_IF_CLEANUP(p.size != 1048576ULL * 512U, inkwell_test_remove_tree(dir),
                                 "the size, from sectors");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_block_find(dir, "misc", &p) != 0 || p.minor != 1U,
                                 inkwell_test_remove_tree(dir), "misc not found");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_block_find(dir, "userdata", &p) != -ENOENT,
                                 inkwell_test_remove_tree(dir), "a name nothing has");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_block_find(dir, "", &p) != -EINVAL,
                                 inkwell_test_remove_tree(dir), "an empty name");
    /* A second disk with the same table: the name no longer says which. */
    INKWELL_TEST_FAIL_IF_CLEANUP(add_entry(dir, "vdb2",
                                           "MAJOR=254\nMINOR=18\nDEVNAME=vdb2\n"
                                           "PARTNAME=system_a\n",
                                           "8\n") != 0,
                                 inkwell_test_remove_tree(dir), "could not add a second disk");
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_block_find(dir, "system_a", &p) != -EEXIST,
                                 inkwell_test_remove_tree(dir), "two partitions with one name");
    inkwell_test_remove_tree(dir);
    record_success(test_name);
}

INKWELL_TEST_CASE(block_find_without_sysfs_says_so, unit) {
    struct inkwell_block_partition p;
    INKWELL_TEST_FAIL_IF(inkwell_block_find("/nonexistent-sys", "misc", &p) != -ENOENT,
                         "a missing tree");
    record_success(test_name);
}
#endif
