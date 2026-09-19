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

# The same core with the CRT linked statically, for IsoAPO.dll (stage 6).
#
# audiodg loads the APO as LocalService, and a DLL that needs MSVCP140 and
# VCRUNTIME140 there fails on any machine without the redistributable, in a
# process that takes all system audio with it. With /MT the DLL imports nothing
# but ole32, shell32, advapi32 and kernel32; it costs about 1.4 MB.
#
# A second target rather than a property on the first: /MT and /MD static
# libraries cannot both be linked into one binary, and the UI links Qt, which is
# /MD. Nothing crosses a heap boundary between the two, because everything the
# APO hands out is COM-allocated.
if(MSVC)
    add_library(isotone_core_mt STATIC
        src/apo_config.cpp
        src/audio_ring.cpp
        src/biquad.cpp
        src/curve_import.cpp
        src/param_block.cpp
        src/processor.cpp
        src/response.cpp
        src/speakers.cpp
    )
    target_include_directories(isotone_core_mt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(isotone_core_mt PUBLIC cxx_std_20)
    set_property(TARGET isotone_core_mt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
