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

# ---------------------------------------------------------------------------
# Installing (stage 6)
#
# isotone-state ships with the daemon: it is how a sink's saved state is read
# or written with nothing running, which is the first thing to reach for when
# the EQ is not being applied.
install(TARGETS isotone-daemon isotone-state
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT engine)

# A user unit, not a system one, and left disabled: starting the daemon moves
# every application's audio through Isotone, which is not something a package
# does to a machine on its own (decisions.md). The app offers to start it.
#
# /usr/lib/systemd/user rather than ${CMAKE_INSTALL_LIBDIR}: that expands to
# lib/x86_64-linux-gnu on Debian, where systemd does not look.
install(FILES isotone-daemon.service
        DESTINATION "lib/systemd/user" COMPONENT engine)
