# Windows side of the shared-memory transport: the named mapping that holds a
# device's ParamBlock and audio ring, and the file that persists the block. Linked
# by the APO, its self test, and later the addon.
add_library(isotone_transport STATIC shared_mapping.cpp persisted_state.cpp)

target_include_directories(isotone_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_transport PUBLIC isotone_core PRIVATE advapi32 shell32 ole32)

# The /MT half, linked by IsoAPO.dll alone (core/CMakeLists.txt says why).
add_library(isotone_transport_mt STATIC shared_mapping.cpp persisted_state.cpp)
target_include_directories(isotone_transport_mt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_transport_mt PUBLIC isotone_core_mt PRIVATE advapi32 shell32 ole32)
set_property(TARGET isotone_transport_mt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
