# inkwell is built two ways and has to be right in both: on its own, where it owns the warning
# flags and builds its tests, and as an add_subdirectory() of an application or of inkcell, where
# it must not reach into the parent's targets or install anything. INKWELL_IS_TOP_LEVEL is what
# tells them apart - CMAKE_SOURCE_DIR equals this directory only in the first case.
cmake_minimum_required(VERSION 3.18)
project(inkwell VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(INKWELL_IS_TOP_LEVEL ON)
else()
    set(INKWELL_IS_TOP_LEVEL OFF)
endif()

option(INKWELL_BUILD_TESTS "Build inkwell's own test binary" ${INKWELL_IS_TOP_LEVEL})
option(INKWELL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(INKWELL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# The same INTERFACE-target trick inkcell and mesh-client use, for the same reason:
# -fsanitize=address has to reach the compile *and* the link, and a variable holding one of them
# can be applied to a new target without the other. Always linked PRIVATE - this is how inkwell
# compiles, not something a consumer inherits.
add_library(inkwell_options INTERFACE)
target_compile_options(inkwell_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wformat=2
)

if(INKWELL_ENABLE_ASAN)
    target_compile_options(inkwell_options INTERFACE -fsanitize=address)
    target_link_options(inkwell_options INTERFACE -fsanitize=address)
endif()

# -fno-sanitize-recover because UBSan otherwise prints a diagnostic and carries on, so the test
# that found the undefined behaviour still passes and CI still goes green.
if(INKWELL_ENABLE_UBSAN)
    target_compile_options(inkwell_options INTERFACE
                           -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(inkwell_options INTERFACE -fsanitize=undefined)
endif()

add_library(inkwell STATIC
    # base: the leaves. Nothing in inkwell includes upward into these, and they include nothing
    # of inkwell's but each other.
    src/base/env.c
    src/base/file.c
    src/base/log.c
    src/base/text.c
    src/base/time.c
    src/base/version.c
    # runtime: the one epoll loop everything else is driven from, and the signals that stop it
    src/runtime/crash.c
    src/runtime/loop.c
    src/runtime/signals.c
    # codec: bytes in, bytes out. They parse; they do not know what the bytes are for.
    src/codec/base64.c
    src/codec/http.c
    src/codec/inflate.c
    src/codec/json.c
    src/codec/mqtt.c
    src/codec/png.c
    src/codec/wuffs.c
    src/codec/sha256.c
    src/codec/zip.c
    # net: one hostname, one socket, one session - all of it on the loop, none of it blocking
    src/net/reason.c
    src/net/resolve.c
)

# Wuffs, for codec/png.c and codec/inflate.c. See third_party/wuffs/README.md for what it is and
# why it is a vendored file rather than a submodule; third_party/wuffs-config/inkwell_wuffs.h is
# what keeps the thirty codecs inkwell does not use out of the binary.
#
# PRIVATE: a consumer includes inkwell/codec/png.h, which says nothing in Wuffs' vocabulary, and
# must not be able to reach the vendored header by accident. Only src/codec/wuffs.c compiles the
# implementation, and it is the one source here built without this project's warning flags -
# a generated 3.6 MB file is not ours to make -Wconversion-clean, and a local edit to silence one
# is exactly what check-vendor.py exists to catch.
target_include_directories(inkwell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wuffs
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wuffs-config)
set_source_files_properties(src/codec/wuffs.c PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(inkwell PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(inkwell PRIVATE inkwell_options)
add_library(inkwell::inkwell ALIAS inkwell)

# _POSIX_C_SOURCE for signalfd, timerfd, eventfd, clock_gettime and the rest of the loop's
# vocabulary. PUBLIC because a consumer that includes runtime/loop.h and then calls epoll
# needs the same declarations visible.
target_compile_definitions(inkwell PUBLIC _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

if(INKWELL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
