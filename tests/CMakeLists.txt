# One binary built from many translation units: a framework and one file per subject area under
# suites/.
#
# Cases register themselves from constructors (see framework/inkwell_test.h), which is why the
# suite files are listed here directly instead of being collected into a static library first: a
# linker is free to drop library members nothing references, and it would take every test inside
# them with it, silently. Add a new suite file to INKWELL_TEST_SUITES and it runs.
set(INKWELL_TEST_SUPPORT
    support/data_fixture.c
    support/fs_fixture.c
    # A real TLS server on a loopback socket, for net_fetch.c. Compiles to nothing without
    # INKWELL_HAVE_TLS, so it is listed unconditionally.
    support/https_fixture.c
    # The certificate and key both TLS fixtures present: https_fixture.c and net_tls_windows.c.
    support/tls_identity.c
)

set(INKWELL_TEST_IO_SUITES suites/io_serial.c suites/io_usb_storage.c)
if(WIN32)
    set(INKWELL_TEST_IO_SUITES suites/io_serial_windows.c suites/io_windows_unavailable.c)
endif()

set(INKWELL_TEST_SUITES
    suites/base_fd_socket.c
    suites/ble_central.c
    suites/ble_hci.c
    suites/base_file.c
    suites/base_log.c
    suites/base_record_file.c
    suites/base_text.c
    suites/base_time.c
    suites/base_version.c
    suites/codec_base64.c
    suites/codec_esp_image.c
    suites/codec_http.c
    suites/codec_inflate.c
    suites/codec_json.c
    suites/codec_mqtt.c
    suites/codec_png.c
    suites/codec_sha256.c
    suites/codec_uf2.c
    suites/codec_zip.c
    ${INKWELL_TEST_IO_SUITES}
    suites/net_fetch.c
    suites/net_mqtt.c
    suites/net_reason.c
    suites/net_resolve.c
    suites/net_stream.c
    suites/net_tcp.c
    suites/net_zip_fetch.c
    suites/runtime_crash.c
    suites/runtime_loop.c
    suites/runtime_timer.c
    suites/runtime_wake.c
)
if(WIN32)
    list(APPEND INKWELL_TEST_SUITES suites/base_fd_device_windows.c suites/net_mqtt_windows.c
         suites/net_resolve_windows.c suites/net_tcp_windows.c suites/net_stream_windows.c
         suites/net_tls_windows.c suites/runtime_windows_socket.c)
endif()

add_executable(inkwell_tests
    framework/inkwell_test.c
    ${INKWELL_TEST_SUPPORT}
    ${INKWELL_TEST_SUITES}
)

target_include_directories(inkwell_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(inkwell_tests PRIVATE inkwell inkwell_options)

# The *source* directory, so a fixture refreshed on disk is the one the next run reads: nothing
# is copied at configure time.
target_compile_definitions(inkwell_tests PRIVATE
                           INKWELL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME inkwell_unit COMMAND inkwell_tests)
set_tests_properties(inkwell_unit PROPERTIES LABELS unit)

# The full suite still has POSIX-only fixtures. Keep the native Windows networking contracts
# runnable independently while those fixtures are ported.
if(WIN32)
    add_executable(inkwell_windows_net_tests framework/inkwell_test.c support/tls_identity.c
                   suites/base_fd_device_windows.c suites/ble_central.c suites/io_serial_windows.c
                   suites/io_windows_unavailable.c suites/net_mqtt_windows.c
                   suites/net_resolve_windows.c suites/net_tls_windows.c)
    target_include_directories(inkwell_windows_net_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(inkwell_windows_net_tests PRIVATE inkwell inkwell_options)
    add_test(NAME inkwell_windows_net COMMAND inkwell_windows_net_tests)
    set_tests_properties(inkwell_windows_net PROPERTIES LABELS unit)
endif()

# The BlueZ backend's D-Bus half, against a fake org.bluez on an isolated bus. Its own
# executable because it needs dbus-run-session around it, and only where both the backend and
# that tool exist: it must never find the system bus.
if(INKWELL_BLE_BACKEND STREQUAL "bluez")
    find_program(INKWELL_DBUS_RUN_SESSION dbus-run-session)
    if(INKWELL_DBUS_RUN_SESSION)
        add_executable(inkwell_ble_bluez_bus_tests ble_bluez_bus.c)
        target_include_directories(inkwell_ble_bluez_bus_tests PRIVATE ${INKWELL_DBUS_INCLUDE_DIRS})
        target_link_libraries(inkwell_ble_bluez_bus_tests PRIVATE inkwell inkwell_options
                              ${INKWELL_DBUS_LIBRARIES})
        add_test(NAME inkwell_ble_bluez_bus
                 COMMAND ${INKWELL_DBUS_RUN_SESSION} -- $<TARGET_FILE:inkwell_ble_bluez_bus_tests>)
        set_tests_properties(inkwell_ble_bluez_bus PROPERTIES LABELS integration TIMEOUT 15)
    endif()
endif()

# The layering rule the compiler cannot see. See scripts/check-layers.py.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME inkwell_layers
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-layers.py)
    set_tests_properties(inkwell_layers PROPERTIES LABELS lint)

    # A vendored third-party file is upstream's, not ours. third_party/wuffs/wuffs-v0.4.c is
    # 3.6 MB of generated C that no review will ever read, so an edit in place is invisible -
    # and a fix that lives only here is a fix nobody upstream knows about. check-vendor.py
    # re-checks it against the digest its own README states, which is also the copy a human
    # reads.
    add_test(NAME inkwell_vendored_files
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-vendor.py)
    set_tests_properties(inkwell_vendored_files PROPERTIES LABELS lint)
endif()
