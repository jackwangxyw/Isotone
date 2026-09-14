// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "shared_mapping.h"

#include <sddl.h>

#include <cwctype>

namespace isotone::win {

std::wstring canonical_endpoint_guid(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n\f\v");
    if (first == std::wstring::npos) return {};
    std::wstring s = text.substr(first, text.find_last_not_of(L" \t\r\n\f\v") - first + 1);
    // A device ID: "{flow and state}." before the braced GUID.
    if (s.size() > 38) {
        const size_t prefix = s.size() - 38;
        if (prefix < 3 || s.front() != L'{' || s.compare(prefix - 2, 2, L"}.") != 0) return {};
        s.erase(0, prefix);
    }
    if (s.size() == 36) s = L"{" + s + L"}";
    // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    if (s.size() != 38 || s.front() != L'{' || s.back() != L'}') return {};
    for (size_t i = 1; i < 37; ++i) {
        const wchar_t c = static_cast<wchar_t>(std::towlower(s[i]));
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? c != L'-' : !((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return {};
        s[i] = c;
    }
    return s;
}

std::wstring mapping_name(const wchar_t* object_namespace, const std::wstring& endpoint) {
    const std::wstring guid = canonical_endpoint_guid(endpoint);
    return guid.empty() ? std::wstring() : object_namespace + (L"IsoAPO." + guid);
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
    // An empty name would open or create an unnamed mapping no one else can reach.
    if (name.empty()) {
        return ERROR_INVALID_NAME;
    }
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
