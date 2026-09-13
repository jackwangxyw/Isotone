# isotone-shm: drives a device's shared region from the UI side, for the stage 3
# acceptance test and for inspecting a live engine.
add_executable(isotone-shm main.cpp)
target_link_libraries(isotone-shm PRIVATE isotone_core isotone_transport)
