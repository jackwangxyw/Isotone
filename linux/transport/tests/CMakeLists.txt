# The transport's naming, its shared region and its saved-state file, without
# PipeWire or a daemon. A directory of its own so it is added after
# enable_testing().
if(ISOTONE_BUILD_TESTS)
    add_executable(transport_posix_tests test_main.cpp test_transport.cpp)
    target_link_libraries(transport_posix_tests PRIVATE isotone_transport_posix)
    target_include_directories(transport_posix_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    add_test(NAME transport_posix_tests COMMAND transport_posix_tests)
endif()
