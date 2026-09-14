// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "persisted_state.h"

#include <knownfolders.h>
#include <shlobj.h>

#include "shared_mapping.h"

namespace isotone::win {

namespace {

// Only the parameters travel: the header's seqlock and host fields describe a
// live region.
ParamBlock parameters_only(const ParamBlock& block) {
    ParamBlock copy = block;
    copy.hdr.seq = 0;
    copy.hdr.sample_rate = 0;
    copy.hdr.channels = 0;
    copy.hdr.host_state = 0;
    copy.hdr.host_heartbeat = 0;
    copy.hdr.speaker_mask = 0;
    for (uint32_t& r : copy.hdr.host_reserved) r = 0;
    return copy;
}

}  // namespace

std::wstring persisted_state_dir(bool selftest) {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(selftest ? FOLDERID_LocalAppData : FOLDERID_ProgramData, 0, nullptr, &base))) {
        dir = base;
        dir += selftest ? L"\\IsoAPO-selftest\\devices" : L"\\IsoAPO\\devices";
    }
    CoTaskMemFree(base);
    return dir;
}

std::wstring persisted_state_path(const std::wstring& dir, const std::wstring& endpoint) {
    const std::wstring guid = canonical_endpoint_guid(endpoint);
    return guid.empty() ? std::wstring() : dir + L"\\" + guid + L".bin";
}

PersistedRead read_persisted_state(const std::wstring& path, ParamBlock* out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PersistedRead::Absent
                                                                              : PersistedRead::Invalid;
    }
    LARGE_INTEGER size{};
    DWORD read = 0;
    ParamBlock block{};
    const bool ok = GetFileSizeEx(file, &size) && size.QuadPart == sizeof(ParamBlock) &&
                    ReadFile(file, &block, sizeof(ParamBlock), &read, nullptr) && read == sizeof(ParamBlock);
    CloseHandle(file);
    if (!ok || !param_block_valid(block)) {
        return PersistedRead::Invalid;
    }
    *out = parameters_only(block);
    return PersistedRead::Loaded;
}

DWORD write_persisted_state(const std::wstring& path, const ParamBlock& block) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return ERROR_BAD_PATHNAME;
    const std::wstring dir = path.substr(0, slash);
    const size_t parent = dir.find_last_of(L'\\');
    if (parent != std::wstring::npos) CreateDirectoryW(dir.substr(0, parent).c_str(), nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return GetLastError();
    }

    const std::wstring temp = path + L".tmp";
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    // The header is this build's whatever the caller's block holds: a block
    // filled by to_param_block alone has none, and would be read back Invalid.
    ParamBlock copy = parameters_only(block);
    copy.hdr.magic = kParamMagic;
    copy.hdr.version = kParamVersion;
    copy.hdr.size = sizeof(ParamBlock);
    DWORD written = 0;
    const bool ok = WriteFile(file, &copy, sizeof(copy), &written, nullptr) && written == sizeof(copy) &&
                    FlushFileBuffers(file);
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return write_error;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        return error;
    }
    return ERROR_SUCCESS;
}

}  // namespace isotone::win
