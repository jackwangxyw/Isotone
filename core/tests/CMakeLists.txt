add_executable(core_tests
    test_main.cpp
    test_apo_config.cpp
    test_audio_ring.cpp
    test_biquad.cpp
    test_param_block.cpp
    test_processor.cpp
    test_smoothing.cpp
    test_response.cpp
    test_reference.cpp
)

target_link_libraries(core_tests PRIVATE isotone_core)
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# doctest 2.4.12 specialises std::tuple, which MSVC 19.51 warns about (C5285).
# Vendored third-party code; nothing for us to fix.
if(MSVC)
    target_compile_options(core_tests PRIVATE /wd5285)
endif()
target_compile_definitions(core_tests PRIVATE
    ISOTONE_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference"
)

add_test(NAME core_tests COMMAND core_tests)
