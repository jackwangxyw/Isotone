// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "eapo_install.h"

#include <cstring>
#include <string>
#include <system_error>

namespace isotone::compat {
namespace {

LSTATUS read_string(HKEY key, const wchar_t* name, std::wstring* out) {
    DWORD bytes = 0;
    LSTATUS s = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                             nullptr, &bytes);
    if (s != ERROR_SUCCESS) return s;
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    s = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(),
                     &bytes);
    if (s != ERROR_SUCCESS) return s;
    value.resize(wcsnlen(value.c_str(), value.size()));
    *out = value;
    return ERROR_SUCCESS;
}

}  // namespace

EqualizerApoInstall locate_equalizer_apo() {
    EqualizerApoInstall r;
    HKEY key = nullptr;
    // The installer is 64-bit on 64-bit Windows; read that view explicitly so a
    // 32-bit build of a tool finds the same key.
    r.error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0,
                            KEY_READ | KEY_WOW64_64KEY, &key);
    if (r.error != ERROR_SUCCESS) return r;

    std::wstring install, config;
    if (read_string(key, L"InstallPath", &install) == ERROR_SUCCESS) r.install_path = install;
    r.error = read_string(key, L"ConfigPath", &config);
    if (r.error == ERROR_SUCCESS) r.config_path = config;
    RegCloseKey(key);
    if (r.error != ERROR_SUCCESS && !r.install_path.empty()) {
        // The install is there even though ConfigPath could not be read.
        r.config_path = r.install_path / "config";
    }
    return r;
}

namespace {

struct Identity {
    DWORD volume = 0;
    FILE_ID_128 id{};
};

bool identity_of(const std::filesystem::path& p, Identity* out) {
    // FILE_FLAG_BACKUP_SEMANTICS opens directories; no access rights are asked
    // for beyond reading attributes, so this touches nothing.
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info)) != 0;
    CloseHandle(h);
    if (!ok) return false;
    out->volume = static_cast<DWORD>(info.VolumeSerialNumber);
    out->id = info.FileId;
    return true;
}

bool same(const Identity& a, const Identity& b) {
    return a.volume == b.volume && std::memcmp(a.id.Identifier, b.id.Identifier, sizeof(a.id.Identifier)) == 0;
}

}  // namespace

bool path_is_inside(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    Identity root_id;
    if (root.empty() || !identity_of(root, &root_id)) return false;   // no such root, nothing to protect

    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(candidate, ec).lexically_normal();
    if (ec) return true;   // cannot tell where it points: refuse rather than guess
    for (;;) {
        Identity id;
        if (identity_of(p, &id) && same(id, root_id)) return true;
        const std::filesystem::path parent = p.parent_path();
        if (parent.empty() || parent == p) return false;
        p = parent;
    }
}

bool is_live_install_path(const std::filesystem::path& dir) {
    // Whatever part of the registration can be read is protected: a missing
    // ConfigPath must not open the guard while InstallPath still says where the
    // install is.
    const EqualizerApoInstall install = locate_equalizer_apo();
    return path_is_inside(dir, install.config_path) || path_is_inside(dir, install.install_path) ||
           (!install.install_path.empty() && path_is_inside(dir, install.install_path / "config"));
}

bool same_file_object(const std::filesystem::path& a, const std::filesystem::path& b) {
    Identity ia, ib;
    return identity_of(a, &ia) && identity_of(b, &ib) && same(ia, ib);
}

}  // namespace isotone::compat
