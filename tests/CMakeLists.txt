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
)

set(INKWELL_TEST_SUITES
    suites/base_log.c
    suites/base_text.c
    suites/base_time.c
    suites/base_version.c
    suites/codec_base64.c
    suites/codec_http.c
    suites/codec_inflate.c
    suites/codec_json.c
    suites/codec_mqtt.c
    suites/codec_png.c
    suites/codec_sha256.c
    suites/codec_zip.c
    suites/net_resolve.c
    suites/runtime_crash.c
    suites/runtime_loop.c
)

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
