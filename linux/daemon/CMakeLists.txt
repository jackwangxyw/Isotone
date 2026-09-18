# The Linux host: the daemon that owns the virtual sink, runs the core over it,
# and plays into a hardware sink. Needs libpipewire, so the top-level list file
# adds this directory only when pkg-config finds it.
add_library(isotone_daemon STATIC daemon.cpp)
target_include_directories(isotone_daemon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isotone_daemon PUBLIC isotone_core isotone_transport_posix
                      PRIVATE PkgConfig::PIPEWIRE)

add_executable(isotone-daemon main.cpp)
target_link_libraries(isotone-daemon PRIVATE isotone_daemon)

# isotone-state: the Linux counterpart of windows/shmtool's isotone-shm. Needs no
# PipeWire, only the transport, so it can inspect a sink with no daemon running.
add_executable(isotone-state state_tool.cpp)
target_link_libraries(isotone-state PRIVATE isotone_core isotone_transport_posix)
