# Windows-only measurement CLI. Not part of the core, and not built on Linux.
add_executable(isotone-measure main.cpp)

target_link_libraries(isotone-measure PRIVATE ole32 oleaut32 uuid winmm)
set_target_properties(isotone-measure PROPERTIES OUTPUT_NAME "isotone-measure")
