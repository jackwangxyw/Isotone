cmake_minimum_required(VERSION 3.20)
# Before project(): with MSVC, project() sets an empty build type to Debug, and a
# Debug IsoAPO.dll imports runtime DLLs no redistributable installs. Multi-config
# generators ignore it.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")
endif()
project(isotone VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# For editors: the build's own flags and include paths (Ninja and Makefile generators).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ISOTONE_BUILD_TESTS "Build the core test suite" ON)
# CI turns this on, so a new warning fails the build. Vendored code opts out
# where it is compiled: doctest is a SYSTEM include and devicetool's upstream/
# sources take /WX-. Headers the compiler treats as system or external (the
# Windows SDK, the MSVC STL, libstdc++) do not warn at all.
option(ISOTONE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /fp:precise)
    # Every header found through INCLUDE (the Windows SDK and the MSVC STL, set by
    # vcvars or by MSBuild) is external, and external headers do not warn.
    add_compile_options(/external:env:INCLUDE /external:W0)
    if(ISOTONE_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ISOTONE_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    # Denormals-are-zero is an SSE3 instruction, and GCC/Clang only expose the
    # intrinsic when the target allows it. Every x86-64 part since about 2005 has
    # SSE3, and without it a filter ringing down into denormals costs roughly a
    # hundred times as much per sample (plan 4.3).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
        add_compile_options(-msse3)
    endif()
endif()

# isotone_version_resource(): the VERSIONINFO shipped Windows binaries carry.
if(WIN32 AND MSVC)
    include(windows/version.cmake)
endif()

add_subdirectory(core)

# The Windows hosts need the MSVC toolchain: the APO links SDK libraries that
# only ship import libs for MSVC, and both use SDK headers MinGW does not carry.
# A MinGW build of the core is still useful as a stand-in for the Linux CI job,
# so gate on the compiler rather than on the platform.
if(WIN32 AND MSVC)
    add_subdirectory(windows/transport)
    add_subdirectory(windows/apo)
    add_subdirectory(windows/shmtool)
    add_subdirectory(windows/devicetool)
endif()

if(ISOTONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()

# After enable_testing(), so their tests are registered with CTest.
if(WIN32 AND MSVC)
    add_subdirectory(windows/transport/tests)
    add_subdirectory(windows/devicetool/tests)
    add_subdirectory(windows/compat)
    add_subdirectory(windows/measure)
    add_subdirectory(windows/devices)
endif()

# The Linux host. Gated on libpipewire actually being installed, so the Windows
# builds and the MinGW stand-in are unchanged, and so a Linux build without the
# development package still configures (CI's core job does not install it).
if(UNIX AND NOT APPLE)
    # The transport needs only POSIX shared memory, so it and its tests build on
    # any Linux, with or without the PipeWire development package. This block is
    # after enable_testing(), so those tests are registered with CTest.
    add_subdirectory(linux/transport)
    add_subdirectory(linux/transport/tests)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
        if(PIPEWIRE_FOUND)
            message(STATUS "libpipewire-0.3 ${PIPEWIRE_VERSION}: building the Linux host")
            add_subdirectory(linux/spike)
            add_subdirectory(linux/daemon)
        else()
            message(STATUS "libpipewire-0.3 not found: skipping the Linux host")
        endif()
    endif()
endif()

# The UI needs Qt 6.11 with its MSVC kit: pass
# -DISOTONE_BUILD_UI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64.
option(ISOTONE_BUILD_UI "Build the Qt Quick UI" OFF)
if(ISOTONE_BUILD_UI)
    if(WIN32 AND NOT MSVC)
        message(FATAL_ERROR "the UI needs the MSVC toolchain on Windows")
    endif()
    add_subdirectory(ui)
endif()
