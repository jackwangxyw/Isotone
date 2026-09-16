add_library(isotone_core STATIC
    src/apo_config.cpp
    src/audio_ring.cpp
    src/biquad.cpp
    src/curve_import.cpp
    src/param_block.cpp
    src/processor.cpp
    src/response.cpp
    src/speakers.cpp
)

target_include_directories(isotone_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(isotone_core PUBLIC cxx_std_20)

add_library(isotone::core ALIAS isotone_core)
