# Windows-only measurement CLI. Not part of the core, and not built on Linux.
add_library(isotone_measure STATIC measure.cpp)
target_include_directories(isotone_measure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_measure PUBLIC ole32 oleaut32 uuid)

add_executable(isotone-measure main.cpp)

target_link_libraries(isotone-measure PRIVATE isotone_measure winmm)
set_target_properties(isotone-measure PROPERTIES OUTPUT_NAME "isotone-measure")

if(ISOTONE_BUILD_TESTS)
    # The window accounting and analysis against fake audio clients. Opens no device.
    add_executable(measure_tests tests/test_measure.cpp)
    target_link_libraries(measure_tests PRIVATE isotone_measure)
    # SYSTEM: doctest is vendored, so its warnings are not ours to fail on.
    target_include_directories(measure_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/core/tests/third_party)
    add_test(NAME measure_tests COMMAND measure_tests)
endif()
