# Equalizer APO compatibility backend (plan 5.5): drives a stock Equalizer APO
# install through its config files, and captures a spectrum with WASAPI
# loopback. No UI code; any front end links this.
add_library(isotone_compat STATIC
    compat_writer.cpp
    config_files.cpp
    eapo_install.cpp
    isotone_file.cpp
    loopback_capture.cpp
)
target_include_directories(isotone_compat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_compat PUBLIC isotone_core PRIVATE advapi32 ole32 shell32 uuid)

add_executable(isotone-compat main.cpp)
target_link_libraries(isotone-compat PRIVATE isotone_compat)

if(ISOTONE_BUILD_TESTS)
    add_executable(compat_tests tests/test_main.cpp tests/test_compat.cpp)
    target_link_libraries(compat_tests PRIVATE isotone_compat ole32)
    target_include_directories(compat_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    # sim/ is gitignored; the test that reads it skips itself when it is absent.
    # The CLI is run from the tests as a second process.
    target_compile_definitions(compat_tests PRIVATE
        ISOTONE_SIM_REFERENCE="${PROJECT_SOURCE_DIR}/sim/reference"
        ISOTONE_COMPAT_EXE="$<TARGET_FILE:isotone-compat>")
    add_dependencies(compat_tests isotone-compat)
    if(MSVC)
        target_compile_options(compat_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME compat_tests COMMAND compat_tests)
endif()
