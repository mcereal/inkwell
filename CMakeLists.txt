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
    src/net/fetch.c
    src/net/reason.c
    src/net/resolve.c
    src/net/stream.c
    src/net/tls.c
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

# Mbed TLS, for net/tls.c and therefore net/fetch.c.
#
# The only optional thing inkwell builds, and optional by *presence*: `INKWELL_HAVE_TLS` is what
# src/net/tls.c switches on, and without the submodule it compiles to a stub that reports TLS
# unavailable and refuses to start a session. Nothing that uses it needs an #ifdef, and net/fetch.c
# still links - it reports itself unavailable, which is what a caller that never means to connect
# anywhere wants anyway.
#
# So a clone that skipped `git submodule update --init` is a complete, buildable, testable inkwell
# without 53 MB of somebody else's C in it. That is the deal this dependency is taken on: a
# platform layer may have one, and it may not make everyone who clones it pay for one.
#
# `TARGET mbedtls` is the same arrangement inkcell uses for inkwell itself: an application that
# carries its own pin adds it first and that pin is what the whole tree builds, rather than two
# copies of the library disagreeing about a version.
#
# It is the *first* half of the condition, not just the guard around add_subdirectory(), and that
# distinction is the whole of it. Such an application has no reason to initialise inkwell's nested
# submodule as well - it already has one - so testing the checkout first would take the stub
# branch and quietly turn TLS off for the entire tree, with the pin sitting right there unused.
# The file check belongs only where a checkout is actually about to be read.
if(TARGET mbedtls OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt)
    if(NOT TARGET mbedtls)
        # Mbed TLS builds its own tests, sample programs and package config by default, none of
        # which belong in this build. FATAL_WARNINGS is turned off because it is that project's
        # policy about its own warnings, applied with this project's compiler flags - a
        # combination neither side tested.
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        # 4.x generates error.c, the SSL debug helpers and the PSA driver wrappers at build time;
        # unlike 3.6 they are not committed to the submodule. Needs Python with jinja2.
        set(GEN_FILES ON CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
        set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)

        # Where this build's two config files live. Mbed TLS 4.x is two projects - TLS/X.509 in
        # one, everything cryptographic in the vendored tf-psa-crypto - and each reads its own
        # user config file. The library applies both to its own targets as PUBLIC compile
        # definitions, so there is nothing to set by hand afterwards and no include directory to
        # add: the value is the full path the generated `#include` uses.
        set(MBEDTLS_USER_CONFIG_FILE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-config/inkwell_mbedtls_config.h
            CACHE FILEPATH "" FORCE)
        set(TF_PSA_CRYPTO_USER_CONFIG_FILE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-config/inkwell_psa_crypto_config.h
            CACHE FILEPATH "" FORCE)

        add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
    endif()

    target_compile_definitions(inkwell PUBLIC INKWELL_HAVE_TLS)
    # PUBLIC, unlike Wuffs. net/tls.h says nothing in Mbed TLS's vocabulary either, but a
    # consumer's *tests* have a real reason to reach the library directly: standing a TLS server
    # on a loopback socket is how a client gets tested without a network, and inkwell's own
    # fixture does exactly that. Hiding the headers would mean every consumer either skipping
    # that test or vendoring a second copy.
    # `tfpsacrypto` is 4.x's name for what 3.x called `mbedcrypto`.
    target_link_libraries(inkwell PUBLIC mbedtls mbedx509 tfpsacrypto)
else()
    message(STATUS
        "third_party/mbedtls is missing: building inkwell without TLS, so net/tls.h refuses every "
        "session and net/fetch.h reports itself unavailable. "
        "Run `git submodule update --init --recursive` to enable it.")
endif()

if(INKWELL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
