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

# ---------------------------------------------------------------------------
# Installing (stage 6)
#
# `cmake --install build --prefix <dir>` lays out everything a package ships,
# and nothing it does not: the diagnostics tools (isotone-shm, isotone-measure,
# isotone-compat), the self tests and the spike are for this repository, not for
# a machine Isotone is installed on.
#
# Windows is flat in the install directory, because that is where the APO must
# be found by full path and where isotone.exe looks for the devicetool beside
# it. Linux follows the usual prefix layout, so a .deb is the same tree under
# /usr (GNUInstallDirs).
include(GNUInstallDirs)
if(WIN32)
    set(ISOTONE_INSTALL_BINDIR ".")
else()
    set(ISOTONE_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
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

# ---------------------------------------------------------------------------
# Packaging (stage 6)
#
# `cpack -G DEB --config build/CPackConfig.cmake` builds the .deb from the same
# install() rules `cmake --install` uses, so the package and a staged tree are
# the same layout.
if(UNIX AND NOT APPLE AND TARGET isotone-daemon)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "isotone")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CONTACT "The Isotone authors <noreply@github.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System-wide parametric EQ")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jackwangxyw/Isotone")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_STRIP_FILES ON)

    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    # Read off the binaries rather than written out, so a Qt or PipeWire version
    # bump does not need this list edited.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # pipewire itself is a runtime program, not a library the linker records.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire, pipewire-pulse | pulseaudio, wireplumber | pipewire-media-session")
    # The extended description only: CPack takes the synopsis from
    # CPACK_PACKAGE_DESCRIPTION_SUMMARY and indents each line of this by one
    # space itself, so a leading space here would show as two.
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"Isotone equalises everything the computer plays. It runs as a PipeWire filter,
so applications need no setting up: their audio goes through it on the way to
the output.
.
Parametric bands with the usual filter types, a response graph with a live
spectrum, per-channel bands, speaker setup with bass management and delay, and
presets that follow the output they were made for.
.
The daemon is enabled on install and starts at the next login.")

    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/linux/packaging/postinst"
        "${CMAKE_CURRENT_SOURCE_DIR}/linux/packaging/prerm")
    # The source tree is on a Windows filesystem when this is built from WSL,
    # which reports every file as 0777; dpkg wants 0755 on a maintainer script,
    # and lintian calls it an error.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

    include(CPack)
endif()

# usr/share/doc: required of a Debian package, and harmless anywhere else.
#
# The changelog has to be gzipped. Done here rather than in an install(CODE)
# block, which is what the first attempt did and which broke under CPack: CPack
# stages through DESTDIR, so ${CMAKE_INSTALL_PREFIX} in an install script is the
# real /usr and writing there is refused. Compressing once at configure time
# sidesteps the question and works the same for `cmake --install`.
if(UNIX AND NOT APPLE AND TARGET isotone-daemon)
    find_program(ISOTONE_GZIP gzip REQUIRED)
    # -n keeps the name and timestamp out of the header, so the package is the
    # same bytes from one build to the next.
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/linux/packaging/changelog"
                "${CMAKE_CURRENT_BINARY_DIR}/changelog"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${ISOTONE_GZIP}" -9 -n -f "${CMAKE_CURRENT_BINARY_DIR}/changelog"
                    COMMAND_ERROR_IS_FATAL ANY)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/linux/packaging/copyright"
                  "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/isotone" COMPONENT app)

    # Man pages, gzipped the same way and for the same reason.
    set(ISOTONE_MAN_GZ "")
    foreach(page isotone isotone-daemon isotone-state)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_CURRENT_SOURCE_DIR}/linux/packaging/man/${page}.1"
                    "${CMAKE_CURRENT_BINARY_DIR}/${page}.1"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${ISOTONE_GZIP}" -9 -n -f "${CMAKE_CURRENT_BINARY_DIR}/${page}.1"
                        COMMAND_ERROR_IS_FATAL ANY)
        list(APPEND ISOTONE_MAN_GZ "${CMAKE_CURRENT_BINARY_DIR}/${page}.1.gz")
    endforeach()
    install(FILES ${ISOTONE_MAN_GZ}
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT app)
endif()
