# The stage 1c spike. PkgConfig::PIPEWIRE is found in the top-level list file,
# which is also what decides whether this directory is added at all.
add_executable(isotone-pw-spike pw_spike.cpp)
target_link_libraries(isotone-pw-spike PRIVATE isotone_core PkgConfig::PIPEWIRE)
