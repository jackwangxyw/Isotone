# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# isotone_version_resource(<target> <description>): gives a Windows target the
# VERSIONINFO in version.rc.in, configured from project(VERSION).
#
# Only shipped binaries get one. Test executables do not: nothing reads their
# version, and a resource on every target would mean regenerating them all on a
# version bump for no purpose.

function(isotone_version_resource target description)
    if(NOT MSVC)
        return()
    endif()

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set(ISOTONE_RC_FILETYPE VFT_DLL)
        set(ISOTONE_RC_FILENAME "${target}.dll")
    else()
        set(ISOTONE_RC_FILETYPE VFT_APP)
        set(ISOTONE_RC_FILENAME "${target}.exe")
    endif()
    set(ISOTONE_RC_NAME "${target}")
    set(ISOTONE_RC_DESCRIPTION "${description}")

    # Per target, because the name and description differ; CMAKE_CURRENT_BINARY_DIR
    # keeps two targets in one directory from writing the same file.
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${PROJECT_SOURCE_DIR}/windows/version.rc.in" "${generated}" @ONLY)
    target_sources(${target} PRIVATE "${generated}")
endfunction()
