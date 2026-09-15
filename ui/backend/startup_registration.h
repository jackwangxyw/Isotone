// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Launch at sign-in (Settings, General): a REG_SZ value under an HKCU key that
// Windows runs at sign-in, the quoted exe path and --tray when Start in the
// tray is on. The key is a parameter so tests use a key of their own; the app
// passes kRunKey.

#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace isotone::ui {

inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValueName[] = L"Isotone";

// "\"<exe>\"", with " --tray" when `tray`.
std::wstring run_command(const std::wstring& exe, bool tray);

// Creates the key under HKEY_CURRENT_USER when it is missing. Win32 error code.
LSTATUS write_run_value(const std::wstring& key, const std::wstring& name, const std::wstring& command);
// Missing key or value: ERROR_SUCCESS.
LSTATUS remove_run_value(const std::wstring& key, const std::wstring& name);
std::optional<std::wstring> read_run_value(const std::wstring& key, const std::wstring& name);

}  // namespace isotone::ui
