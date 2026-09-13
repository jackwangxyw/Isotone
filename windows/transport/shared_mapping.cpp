// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "shared_mapping.h"

#include <sddl.h>

#include <cwctype>

namespace isotone::win {

std::wstring mapping_name(const wchar_t* object_namespace, const std::wstring& endpoint_guid) {
    std::wstring name = object_namespace;
    name += L"IsoAPO.";
    for (wchar_t c : endpoint_guid) {
        name += static_cast<wchar_t>(std::towlower(c));
    }
    return name;
}

DWORD SharedMapping::create_or_open(const std::wstring& name, void (*seed)(ParamBlock* block, void* context),
                                    void* context) {
    // Open first. CreateFileMappingW on an object that already exists asks for
    // full access, which kMappingSddl grants only to SYSTEM and LocalService;
    // opening needs just the read and write every host actually uses.
    const DWORD open_error = open(name);
    if (open_error != ERROR_FILE_NOT_FOUND) {
        return open_error;
    }

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kMappingSddl, SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        return GetLastError();
    }
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};

    const ULARGE_INTEGER size{.QuadPart = kSharedRegionBytes};
    handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, size.HighPart,
                                 size.LowPart, name.c_str());
    const DWORD error = GetLastError();
    LocalFree(sd);
    if (handle_ == nullptr) {
        // A sibling instance can create the region between the open and the
        // create, which a caller without full access sees as access denied.
        if (error == ERROR_ACCESS_DENIED && open(name) == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }
        return error;
    }

    if (error != ERROR_ALREADY_EXISTS) {
        view_ = MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedRegionBytes);
        if (view_ == nullptr) {
            const DWORD map_error = GetLastError();
            close();
            return map_error;
        }
        // A new mapping is zero-filled by the system, so the region only needs
        // its headers written.
        init_shared_region(view_, seed, context);
        created_ = true;
        return ERROR_SUCCESS;
    }
    return map_existing();
}

DWORD SharedMapping::open(const std::wstring& name) {
    close();
    handle_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (handle_ == nullptr) {
        return GetLastError();
    }
    return map_existing();
}

DWORD SharedMapping::map_existing() {
    view_ = MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (view_ == nullptr) {
        const DWORD error = GetLastError();
        close();
        return error;
    }
    MEMORY_BASIC_INFORMATION info{};
    const size_t bytes = VirtualQuery(view_, &info, sizeof(info)) != 0 ? info.RegionSize : 0;

    // The creator may still be writing the headers. It writes the magic last,
    // so a zero magic means "not finished yet" rather than "wrong layout". The
    // creator finishes within microseconds; a magic still zero after 50 ms was
    // zeroed by someone else, and waiting longer would only stall the caller.
    const ULONGLONG deadline = GetTickCount64() + 50;
    while (bytes >= sizeof(ParamBlock) && params()->hdr.magic == 0 && GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (!shared_region_valid(view_, bytes)) {
        close();
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

void SharedMapping::close() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    created_ = false;
}

}  // namespace isotone::win
