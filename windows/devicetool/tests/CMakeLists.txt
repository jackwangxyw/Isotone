# isotone-devicetool driven as the UI drives it, through isotone_devicetool_session
# unelevated, and directly. Dry runs only. A directory of its own so it is added
# after enable_testing().
if(ISOTONE_BUILD_TESTS)
    add_executable(devicetool_tests test_main.cpp test_devicetool.cpp)
    target_link_libraries(devicetool_tests PRIVATE isotone_devicetool_session)
    target_include_directories(devicetool_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    target_compile_definitions(devicetool_tests PRIVATE ISOTONE_DEVICETOOL_EXE="$<TARGET_FILE:isotone-devicetool>")
    add_dependencies(devicetool_tests isotone-devicetool)
    if(MSVC)
        target_compile_options(devicetool_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME devicetool_tests COMMAND devicetool_tests)
endif()
