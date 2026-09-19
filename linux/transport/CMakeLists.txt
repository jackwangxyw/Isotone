# Linux side of the shared-memory transport: the POSIX shared-memory object that
# holds a sink's ParamBlock and audio ring, the file that persists the block, and
# the daemon's own settings.
# Linked by the daemon and by the UI's Linux backend.
add_library(isotone_transport_posix STATIC shared_region.cpp persisted_state.cpp daemon_config.cpp)

target_include_directories(isotone_transport_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_transport_posix PUBLIC isotone_core)
