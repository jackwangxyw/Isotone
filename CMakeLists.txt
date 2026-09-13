cmake_minimum_required(VERSION 3.20)
project(isotone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(ISOTONE_BUILD_TESTS "Build the core test suite" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /fp:precise)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
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
    add_subdirectory(windows/measure)
    add_subdirectory(windows/shmtool)
endif()

if(ISOTONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(core/tests)
endif()
