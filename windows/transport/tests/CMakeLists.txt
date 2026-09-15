# The transport's naming and saved-state file, without IsoAPO or the registry.
# A directory of its own so it is added after enable_testing().
if(ISOTONE_BUILD_TESTS)
    add_executable(transport_tests test_main.cpp test_transport.cpp)
    target_link_libraries(transport_tests PRIVATE isotone_transport)
    target_include_directories(transport_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    if(MSVC)
        target_compile_options(transport_tests PRIVATE /wd5285)   # doctest, as in core/tests
    endif()
    add_test(NAME transport_tests COMMAND transport_tests)
endif()
