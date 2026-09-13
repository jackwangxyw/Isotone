# Windows side of the shared-memory transport: the named mapping that holds a
# device's ParamBlock and audio ring, and the file that persists the block. Linked
# by the APO, its self test, and later the addon.
add_library(isotone_transport STATIC shared_mapping.cpp persisted_state.cpp)

target_include_directories(isotone_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_transport PUBLIC isotone_core PRIVATE advapi32 shell32 ole32)
