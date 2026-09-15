cmake_minimum_required(VERSION 3.20)
# Before project(): with MSVC, project() sets an empty build type to Debug, and a
# Debug IsoAPO.dll imports runtime DLLs no redistributable installs. Multi-config
# generators ignore it.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")
endif()
project(isotone LANGUAGES CXX)

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
