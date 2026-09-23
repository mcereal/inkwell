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

set(INKWELL_CRASH_SOURCE src/runtime/crash.c)
set(INKWELL_SIGNALS_SOURCE src/runtime/signals.c)
set(INKWELL_WINDOWS_RUNTIME_SOURCE)
set(INKWELL_BASE_PLATFORM_SOURCE src/runtime/base_posix.c)
set(INKWELL_RESOLVE_SOURCE src/net/resolve.c)
if(WIN32)
    set(INKWELL_CRASH_SOURCE src/runtime/crash_windows.c)
    set(INKWELL_SIGNALS_SOURCE src/runtime/signals_windows.c)
    set(INKWELL_WINDOWS_RUNTIME_SOURCE src/runtime/windows_handle.c)
    set(INKWELL_BASE_PLATFORM_SOURCE src/runtime/base_windows.c)
    set(INKWELL_RESOLVE_SOURCE src/net/resolve_unavailable.c)
endif()

add_library(inkwell STATIC
    # base: the leaves. Nothing in inkwell includes upward into these, and they include nothing
    # of inkwell's but each other.
    src/base/env.c
    src/base/fd.c
    src/base/file.c
    src/base/log.c
    src/base/record_file.c
    src/base/text.c
    src/base/time.c
    src/base/version.c
    # runtime: the one loop everything else is driven from, the signals that stop it, and the two
    # descriptors - a timer and a wake - that let something which is not I/O wait on it too
    ${INKWELL_CRASH_SOURCE}
    src/runtime/loop.c
    ${INKWELL_SIGNALS_SOURCE}
    src/runtime/timer.c
    src/runtime/wake.c
    ${INKWELL_BASE_PLATFORM_SOURCE}
    ${INKWELL_WINDOWS_RUNTIME_SOURCE}
    # codec: bytes in, bytes out. They parse; they do not know what the bytes are for.
    src/codec/base64.c
    src/codec/esp_image.c
    src/codec/http.c
    src/codec/inflate.c
    src/codec/json.c
    src/codec/mqtt.c
    src/codec/png.c
    src/codec/wuffs.c
    src/codec/sha256.c
    src/codec/uf2.c
    src/codec/zip.c
    # net: one hostname, one socket, one session - all of it on the loop, none of it blocking
    src/net/fetch.c
    src/net/mqtt.c
    src/net/reason.c
    ${INKWELL_RESOLVE_SOURCE}
    src/net/stream.c
    src/net/tcp.c
    src/net/tls.c
    src/net/zip_fetch.c
    # io: the USB serial ports the system has, and a tty opened raw on the loop's terms
    src/io/serial.c
    src/io/usb_storage.c
    # ble: a Bluetooth LE central over whichever stack the system has. The backend is added
    # below, once the build knows which one that is.
    src/ble/central.c
    src/ble/hci.c
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
if(WIN32)
    target_link_libraries(inkwell PUBLIC ws2_32)
endif()
add_library(inkwell::inkwell ALIAS inkwell)

# _POSIX_C_SOURCE for signalfd, timerfd, eventfd, clock_gettime and the rest of the loop's
# vocabulary. PUBLIC because a consumer that includes runtime/loop.h and then calls epoll
# needs the same declarations visible.
if(NOT WIN32)
    target_compile_definitions(inkwell PUBLIC _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

# macOS is a development host, not a target: the loop is kqueue there, the device-only halves of
# the layers above compile out, and the same UI runs in a window. _POSIX_C_SOURCE on Darwin
# *hides* everything that is not POSIX - kqueue's timer flags, mkdtemp(), SO_NOSIGPIPE - where on
# Linux _DEFAULT_SOURCE puts the rest back, and _DARWIN_C_SOURCE is Darwin's spelling of that.
if(APPLE)
    target_compile_definitions(inkwell PUBLIC _DARWIN_C_SOURCE)
endif()

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
option(INKWELL_WITH_TLS "Build TLS support when Mbed TLS is present" ON)
if(INKWELL_WITH_TLS AND
   (TARGET mbedtls OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt))
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
    if(INKWELL_WITH_TLS)
        message(STATUS
            "third_party/mbedtls is missing: building inkwell without TLS, so net/tls.h refuses "
            "every session and net/fetch.h reports itself unavailable. "
            "Run `git submodule update --init --recursive` to enable it.")
    else()
        message(STATUS "inkwell: TLS support disabled by INKWELL_WITH_TLS=OFF")
    endif()
endif()

# The Bluetooth stack under ble/central.h, chosen here because a process has exactly one.
#
# On Linux that is BlueZ, reached over D-Bus, and optional by presence exactly as Mbed TLS is:
# with libdbus-1's headers the BlueZ backend is built, without them none.c is, and every call
# refuses with -ENOSYS - so a Linux box with no D-Bus development package still builds, links
# and tests everything else, and a consumer reports Bluetooth unavailable rather than failing
# to compile.
#
# On macOS it is CoreBluetooth, which is always there. That backend is Objective-C - the only
# file in inkwell that is - built with ARC, and the one place a second thread exists: the
# framework's own dispatch queue, which src/ble/corebluetooth.m keeps to copies and a wake.
#
# PRIVATE, like Wuffs: ble/central.h says nothing in D-Bus's or CoreBluetooth's vocabulary, and a
# consumer has no reason to reach either through this target.
set(INKWELL_BLE_BACKEND none)
# The serial scan's macOS half reads the I/O Registry. PRIVATE for the same reason as the
# Bluetooth frameworks below: io/serial.h says nothing in IOKit's vocabulary.
if(APPLE)
    target_sources(inkwell PRIVATE src/io/serial_iokit.c)
    target_link_libraries(inkwell PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()

set(INKWELL_BLE_BACKEND_SOURCE src/ble/none.c)
if(APPLE)
    enable_language(OBJC)
    set(INKWELL_BLE_BACKEND corebluetooth)
    set(INKWELL_BLE_BACKEND_SOURCE src/ble/corebluetooth.m)
    set_source_files_properties(src/ble/corebluetooth.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(inkwell PRIVATE "-framework CoreBluetooth" "-framework Foundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(INKWELL_DBUS QUIET dbus-1)
    endif()
    if(INKWELL_DBUS_FOUND)
        set(INKWELL_BLE_BACKEND bluez)
        set(INKWELL_BLE_BACKEND_SOURCE src/ble/bluez.c)
        target_include_directories(inkwell PRIVATE ${INKWELL_DBUS_INCLUDE_DIRS})
        # By name and directory rather than by the full path pkg-config resolves: a static
        # device build passes -static and a cross prefix, and a resolved path to a shared
        # libdbus-1 would defeat both where the bare name lets the linker pick the archive.
        target_link_libraries(inkwell PRIVATE ${INKWELL_DBUS_LIBRARIES})
        target_link_directories(inkwell INTERFACE ${INKWELL_DBUS_LIBRARY_DIRS})
    else()
        message(STATUS
            "libdbus-1 is missing: building inkwell without a Bluetooth stack, so every call in "
            "ble/central.h refuses with -ENOSYS. Install libdbus-1-dev to enable BlueZ.")
    endif()
endif()
target_sources(inkwell PRIVATE ${INKWELL_BLE_BACKEND_SOURCE})
target_compile_definitions(inkwell PUBLIC INKWELL_BLE_BACKEND_${INKWELL_BLE_BACKEND})

if(INKWELL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
