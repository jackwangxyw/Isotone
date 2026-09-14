// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "dry_run.h"

#include <cwctype>

#include <aclapi.h>
#include <sddl.h>

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
    const std::wstring k = normalize(key);
    // A real createKey the key's ACL refuses throws, and upstream's install
    // catches that, takes ownership and makes the key writable. Throw here too,
    // so a dry run shows that path rather than a create that would fail.
    if (operation == L"create key" && create_would_be_denied(key)) {
        throw RegistryException(L"Error while creating registry key " + key +
                                L": access denied (dry run: Administrators may not create subkeys here)");
    }
    if (operations_.size() == kill_at_) throw SimulatedKill{};
    if (const auto it = fail_at_.find(operations_.size()); it != fail_at_.end()) {
        fail_at_.erase(it);
        throw RegistryException(L"dry run: injected failure of " + operation + L" " + key + L"\\" + valuename);
    }
    operations_.push_back({operation, key, valuename, data});
    if (operation == L"grant Administrators KEY_ALL_ACCESS") {
        made_writable_.insert(k);
    }

    if (operation == L"create key") {
        if (deleted(k)) recreated_.insert(k);
        deleted_.erase(k);
        created_.insert(k);
    } else if (operation == L"delete key") {
        created_.erase(k);
        deleted_.insert(k);
        for (auto it = values_.begin(); it != values_.end();) {
            it = it->first.rfind(k + L'|', 0) == 0 ? values_.erase(it) : std::next(it);
        }
    } else if (operation.rfind(L"set ", 0) == 0) {
        const std::wstring kind = operation.substr(4);
        const unsigned long type = kind == L"REG_DWORD"     ? REG_DWORD
                                   : kind == L"REG_MULTI_SZ" ? REG_MULTI_SZ
                                   : kind == L"REG_BINARY"   ? REG_BINARY
                                                             : REG_SZ;
        values_[value_id(key, valuename)] = Value{data, false, type};
    } else if (operation == L"delete value") {
        values_[value_id(key, valuename)] = Value{L"", true};
    }
    // Ownership, ACL and backup operations change nothing a later read sees.
}

bool DryRunRegistry::deleted(const std::wstring& normalized) const {
    for (const std::wstring& d : deleted_) {
        if (normalized == d || normalized.rfind(d + L'\\', 0) == 0) return true;
    }
    // Under a key deleted and recreated, only what was created since exists.
    for (const std::wstring& r : recreated_) {
        if (normalized.rfind(r + L'\\', 0) == 0 && !created_.count(normalized)) return true;
    }
    return false;
}

bool DryRunRegistry::emptied(const std::wstring& normalized) const {
    for (const std::wstring& r : recreated_) {
        if (normalized == r || normalized.rfind(r + L'\\', 0) == 0) return true;
    }
    return false;
}

namespace {

// Whether the key's DACL lets BUILTIN\Administrators create subkeys. Deny ACEs
// are checked first, as Windows evaluates them first in a canonical ACL.
bool admins_may_create_subkeys(const std::wstring& key) {
    HKEY h = nullptr;
    try {
        h = RegistryHelper::openKey(key, READ_CONTROL | KEY_WOW64_64KEY);
    } catch (RegistryException&) {
        return true;   // cannot read the ACL; assume the create works, as upstream's code first tries
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD status = GetSecurityInfo(h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                         nullptr, &sd);
    RegCloseKey(h);
    if (status != ERROR_SUCCESS) return true;
    PSID admins = nullptr;
    ConvertStringSidToSidW(L"S-1-5-32-544", &admins);
    // An ACE counts only if it carries KEY_CREATE_SUB_KEY itself, or a generic
    // right that maps to it. KEY_READ, KEY_SET_VALUE and READ_CONTROL do not.
    GENERIC_MAPPING registry_mapping = {KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS};
    const auto carries_create = [&](ACCESS_MASK mask) {
        MapGenericMask(&mask, &registry_mapping);
        return (mask & KEY_CREATE_SUB_KEY) != 0;
    };
    bool allowed = dacl == nullptr, denied = false;
    for (DWORD i = 0; dacl != nullptr && admins != nullptr && i < dacl->AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!GetAce(dacl, i, &ace)) continue;
        const auto* header = static_cast<ACE_HEADER*>(ace);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) continue;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* a = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            if (EqualSid(static_cast<PSID>(&a->SidStart), admins) && carries_create(a->Mask)) allowed = true;
        } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            auto* d = static_cast<ACCESS_DENIED_ACE*>(ace);
            if (EqualSid(static_cast<PSID>(&d->SidStart), admins) && carries_create(d->Mask)) denied = true;
        }
    }
    if (admins != nullptr) LocalFree(admins);
    if (sd != nullptr) LocalFree(sd);
    return allowed && !denied;
}

}  // namespace

bool DryRunRegistry::create_would_be_denied(const std::wstring& key) const {
    const std::wstring k = normalize(key);
    // A key this dry run deleted is created again, not opened.
    if (created_.count(k) || (!deleted(k) && really_exists(key))) return false;
    // The nearest ancestor that exists, in the registry or in this dry run.
    std::wstring parent = key;
    for (;;) {
        const size_t slash = parent.rfind(L'\\');
        if (slash == std::wstring::npos) return false;
        parent = parent.substr(0, slash);
        const std::wstring np = normalize(parent);
        if (made_writable_.count(np) || created_.count(np)) return false;
        if (!deleted(np) && really_exists(parent)) return !admins_may_create_subkeys(parent);
    }
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
    // the registry about it would throw; nor does one it deleted and recreated.
    if ((created_.count(k) && !really_exists(key)) || emptied(k)) { *result = false; return true; }
    return false;
}

bool DryRunRegistry::readValue(const std::wstring& key, const std::wstring& valuename,
                               std::wstring* result) {
    const auto it = values_.find(value_id(key, valuename));
    if (it != values_.end() && !it->second.removed) { *result = it->second.data; return true; }
    return false;
}

bool DryRunRegistry::readMultiValue(const std::wstring& key, const std::wstring& valuename,
                                    std::vector<std::wstring>* result) {
    const auto it = values_.find(value_id(key, valuename));
    if (it == values_.end() || it->second.removed) return false;
    result->clear();
    const std::wstring& data = it->second.data;
    size_t start = 0;
    while (!data.empty() && start <= data.size()) {
        const size_t bar = data.find(L'|', start);
        result->push_back(data.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return true;
}

bool DryRunRegistry::valueType(const std::wstring& key, const std::wstring& valuename, unsigned long* type,
                               bool* present) const {
    const std::wstring k = normalize(key);
    if (deleted(k)) { *present = false; return true; }
    const auto it = values_.find(value_id(key, valuename));
    if (it != values_.end()) {
        *present = !it->second.removed;
        *type = it->second.type;
        return true;
    }
    if ((created_.count(k) && !really_exists(key)) || emptied(k)) { *present = false; return true; }
    return false;
}

bool DryRunRegistry::keyEmpty(const std::wstring& key, bool* result) {
    const std::wstring k = normalize(key);
    if (deleted_.empty() && created_.empty() && values_.empty()) return false;

    // Real subkeys and values, adjusted for what this dry run did.
    size_t subkeys = 0, values = 0;
    if (really_exists(key) && !emptied(k)) {
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
