// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "dry_run.h"

#include <cwctype>

namespace isotone::devicetool {

namespace {

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Registry paths are case-insensitive; compare them that way.
std::wstring normalize(const std::wstring& key) {
    std::wstring k = lower(key);
    while (!k.empty() && k.back() == L'\\') k.pop_back();
    return k;
}

std::wstring value_id(const std::wstring& key, const std::wstring& valuename) {
    return normalize(key) + L'|' + lower(valuename);
}

// Asks the real registry with the hook out of the way.
class HookPaused {
public:
    HookPaused() : saved_(RegistryHelper::dryRun) { RegistryHelper::dryRun = nullptr; }
    ~HookPaused() { RegistryHelper::dryRun = saved_; }
private:
    RegistryDryRun* saved_;
};

}  // namespace

void DryRunRegistry::write(const std::wstring& operation, const std::wstring& key,
                           const std::wstring& valuename, const std::wstring& data) {
    operations_.push_back({operation, key, valuename, data});
    const std::wstring k = normalize(key);

    if (operation == L"create key") {
        deleted_.erase(k);
        created_.insert(k);
    } else if (operation == L"delete key") {
        created_.erase(k);
        deleted_.insert(k);
        for (auto it = values_.begin(); it != values_.end();) {
            it = it->first.rfind(k + L'|', 0) == 0 ? values_.erase(it) : std::next(it);
        }
    } else if (operation.rfind(L"set ", 0) == 0) {
        values_[value_id(key, valuename)] = Value{data, false};
    } else if (operation == L"delete value") {
        values_[value_id(key, valuename)] = Value{L"", true};
    }
    // Ownership, ACL and backup operations change nothing a later read sees.
}

bool DryRunRegistry::deleted(const std::wstring& normalized) const {
    for (const std::wstring& d : deleted_) {
        if (normalized == d || normalized.rfind(d + L'\\', 0) == 0) return true;
    }
    return false;
}

bool DryRunRegistry::really_exists(const std::wstring& key) const {
    HookPaused paused;
    return RegistryHelper::keyExists(key);
}

bool DryRunRegistry::keyExists(const std::wstring& key, bool* result) {
    const std::wstring k = normalize(key);
    if (deleted(k)) { *result = false; return true; }
    if (created_.count(k)) { *result = true; return true; }
    return false;
}

bool DryRunRegistry::valueExists(const std::wstring& key, const std::wstring& valuename,
                                 bool* result) {
    const std::wstring k = normalize(key);
    if (deleted(k)) { *result = false; return true; }
    const auto it = values_.find(value_id(key, valuename));
    if (it != values_.end()) { *result = !it->second.removed; return true; }
    // A key that only exists in this dry run has no other values, and asking
    // the registry about it would throw.
    if (created_.count(k) && !really_exists(key)) { *result = false; return true; }
    return false;
}

bool DryRunRegistry::readValue(const std::wstring& key, const std::wstring& valuename,
                               std::wstring* result) {
    const auto it = values_.find(value_id(key, valuename));
    if (it != values_.end() && !it->second.removed) { *result = it->second.data; return true; }
    return false;
}

bool DryRunRegistry::keyEmpty(const std::wstring& key, bool* result) {
    const std::wstring k = normalize(key);
    if (deleted_.empty() && created_.empty() && values_.empty()) return false;

    // Real subkeys and values, adjusted for what this dry run did.
    size_t subkeys = 0, values = 0;
    if (really_exists(key)) {
        HookPaused paused;
        for (const std::wstring& child : RegistryHelper::enumSubKeys(key)) {
            if (!deleted(normalize(key + L"\\" + child))) ++subkeys;
        }
        HKEY h = RegistryHelper::openKey(key, KEY_QUERY_VALUE | KEY_WOW64_64KEY);
        DWORD count = 0;
        RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr,
                         nullptr, nullptr, nullptr);
        RegCloseKey(h);
        values = count;
    }
    for (const std::wstring& c : created_) {
        if (c.rfind(k + L'\\', 0) == 0 && c.find(L'\\', k.size() + 1) == std::wstring::npos &&
            !really_exists(c)) {
            ++subkeys;
        }
    }
    for (const auto& [id, v] : values_) {
        if (id.rfind(k + L'|', 0) == 0) {
            // Only count changes relative to the real key; exact reconciliation
            // is not needed for the one caller, uninstall's "is Child APOs empty".
            if (!v.removed) ++values;
        }
    }
    *result = subkeys == 0 && values == 0;
    return true;
}

}  // namespace isotone::devicetool
