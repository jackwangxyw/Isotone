// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "startup_registration.h"

#include <vector>

namespace isotone::ui {

std::wstring run_command(const std::wstring& exe, bool tray) {
    std::wstring command = L"\"" + exe + L"\"";
    if (tray) command += L" --tray";
    return command;
}

LSTATUS write_run_value(const std::wstring& key, const std::wstring& name, const std::wstring& command) {
    HKEY h = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(h, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return status;
}

LSTATUS remove_run_value(const std::wstring& key, const std::wstring& name) {
    HKEY h = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &h);
    if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    status = RegDeleteValueW(h, name.c_str());
    RegCloseKey(h);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

std::optional<std::wstring> read_run_value(const std::wstring& key, const std::wstring& name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    return std::wstring(buffer.data());
}

}  // namespace isotone::ui
