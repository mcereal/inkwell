#define _POSIX_C_SOURCE 200809L

/* Memory two processes can map, and descriptors across a Unix-domain socket. */

#include "framework/inkwell_test.h"

#include "inkwell/base/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

INKWELL_TEST_CASE(fd_shared_memory_is_mapped_by_both_and_cannot_shrink, unit) {
    const size_t size = 8192U;
    const int fd = inkwell_fd_shared_memory(size);
    INKWELL_TEST_FAIL_IF(fd < 0, "a memory object of 8 KiB");
    INKWELL_TEST_FAIL_IF((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0, "born close-on-exec");
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory_check(fd, size) != 0,
                         "it passes the receiver's check at its size");
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory_check(fd, size * 16U) != -EINVAL,
                         "and fails it for more than it holds (macOS rounds up to a page)");

    void *writer = NULL;
    void *reader = NULL;
    INKWELL_TEST_FAIL_IF(inkwell_fd_map(fd, size, true, &writer) != 0, "mapped to write");
    INKWELL_TEST_FAIL_IF(inkwell_fd_map(fd, size, false, &reader) != 0, "mapped again to read");
    INKWELL_TEST_FAIL_IF(((const uint8_t *)reader)[size - 1U] != 0U, "born zeroed");
    memcpy((uint8_t *)writer + 4000U, "shared", 7U);
    INKWELL_TEST_FAIL_IF(strcmp((const char *)reader + 4000U, "shared") != 0,
                         "what one mapping writes, the other reads");
    (void)ftruncate(fd, 0); /* refused by the seal on Linux, by the fixed size on macOS */
    (void)inkwell_fd_unmap(writer, size);
    (void)inkwell_fd_unmap(reader, size);
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory_check(fd, size) != 0,
                         "a producer's attempt to shrink it changed nothing");
    (void)close(fd);

    char path[] = "/tmp/inkwell-fd-XXXXXX";
    const int file = mkstemp(path);
    INKWELL_TEST_FAIL_IF(file < 0 || ftruncate(file, (off_t)size) != 0, "a plain file");
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory_check(file, size) == 0,
                         "a file that could be shrunk under the reader is refused");
    (void)close(file);
    (void)unlink(path);
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory(0U) != -EINVAL, "an empty object is refused");
    record_success(test_name);
}

INKWELL_TEST_CASE(fd_descriptors_cross_a_socket_with_their_bytes, unit) {
    int pair[2];
    INKWELL_TEST_FAIL_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0, "a socket pair");
    const int memory = inkwell_fd_shared_memory(4096U);
    int pipe_ends[2];
    INKWELL_TEST_FAIL_IF(memory < 0 || inkwell_fd_pipe(pipe_ends) != 0, "a memory object, a pipe");
    void *map = NULL;
    INKWELL_TEST_FAIL_IF(inkwell_fd_map(memory, 4096U, true, &map) != 0, "mapped");
    memcpy(map, "passed", 7U);

    const int out[2] = {memory, pipe_ends[1]};
    INKWELL_TEST_FAIL_IF(inkwell_socket_send_fds((inkwell_socket)pair[0], "m", 1U, out, 2U) != 1,
                         "one byte with two descriptors on it");
    INKWELL_TEST_FAIL_IF(inkwell_socket_send((inkwell_socket)pair[0], "xy", 2U) != 2,
                         "then two bytes with none");
    INKWELL_TEST_FAIL_IF(fcntl(memory, F_GETFD) < 0, "the sender keeps its own");

    char bytes[8] = {0};
    struct inkwell_fd_received received;
    const int got =
        inkwell_socket_recv_fds((inkwell_socket)pair[1], bytes, sizeof bytes, &received);
    INKWELL_TEST_FAIL_IF(got < 1 || bytes[0] != 'm', "the byte arrives");
    INKWELL_TEST_FAIL_IF(received.count != 2U || received.lost, "with both descriptors");
    INKWELL_TEST_FAIL_IF((fcntl(received.fds[0], F_GETFD) & FD_CLOEXEC) == 0,
                         "received close-on-exec");

    void *seen = NULL;
    INKWELL_TEST_FAIL_IF(inkwell_fd_shared_memory_check(received.fds[0], 4096U) != 0 ||
                             inkwell_fd_map(received.fds[0], 4096U, false, &seen) != 0,
                         "the received memory checks out and maps");
    INKWELL_TEST_FAIL_IF(strcmp(seen, "passed") != 0, "and holds what the sender wrote");
    INKWELL_TEST_FAIL_IF(write(received.fds[1], "p", 1) != 1, "the received pipe end writes");
    char through = 0;
    INKWELL_TEST_FAIL_IF(read(pipe_ends[0], &through, 1) != 1 || through != 'p',
                         "into the sender's pipe");

    (void)inkwell_fd_unmap(seen, 4096U);
    (void)inkwell_fd_unmap(map, 4096U);
    for (size_t i = 0U; i < received.count; ++i) {
        (void)close(received.fds[i]);
    }
    INKWELL_TEST_FAIL_IF(inkwell_socket_send_fds((inkwell_socket)pair[0], "m", 0U, out, 1U) !=
                             -EINVAL,
                         "descriptors need a byte to ride on");
    const int too_many[5] = {memory, memory, memory, memory, memory};
    INKWELL_TEST_FAIL_IF(inkwell_socket_send_fds((inkwell_socket)pair[0], "m", 1U, too_many, 5U) !=
                             -EINVAL,
                         "more than INKWELL_FD_PASS_MAX is refused");
    (void)close(memory);
    (void)close(pipe_ends[0]);
    (void)close(pipe_ends[1]);
    (void)close(pair[0]);
    (void)close(pair[1]);
    record_success(test_name);
}
