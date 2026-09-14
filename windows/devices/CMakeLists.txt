# Render endpoints for the UI: enumeration, mix format, engine per endpoint,
# change notifications, and whether IsoAPO is running. Read-only.
add_library(isotone_devices STATIC
    devices.cpp
    device_watcher.cpp
    engine_probe.cpp
)
target_include_directories(isotone_devices PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_devices PUBLIC isotone_core isotone_transport PRIVATE advapi32 ole32 uuid)

if(ISOTONE_BUILD_TESTS)
    add_executable(devices_tests tests/test_main.cpp tests/test_devices.cpp)
    target_link_libraries(devices_tests PRIVATE isotone_devices advapi32 ole32)
    target_include_directories(devices_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    # The devicetool the comparison runs; ISOTONE_DEVICETOOL in the environment
    # overrides it. The test skips when the file is missing.
    target_compile_definitions(devices_tests PRIVATE ISOTONE_DEVICETOOL_EXE="$<TARGET_FILE:isotone-devicetool>")
    add_dependencies(devices_tests isotone-devicetool)
    if(MSVC)
        target_compile_options(devices_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME devices_tests COMMAND devices_tests)
    # The skip a machine without audio endpoints takes (a CI runner), forced.
    add_test(NAME devices_tests_no_audio COMMAND devices_tests)
    set_tests_properties(devices_tests_no_audio PROPERTIES ENVIRONMENT "ISOTONE_DEVICES_TEST_NO_AUDIO=1")
endif()
