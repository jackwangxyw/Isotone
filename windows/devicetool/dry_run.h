// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// A RegistryDryRun that records every write upstream's install and uninstall
// would make, and answers the reads they make afterwards as if those writes had
// happened. Nothing reaches the registry while it is installed.

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "helpers/RegistryHelper.h"

namespace isotone::devicetool {

struct RegistryOperation {
    std::wstring operation;
    std::wstring key;
    std::wstring valuename;
    std::wstring data;
};

class DryRunRegistry : public RegistryDryRun {
public:
    void write(const std::wstring& operation, const std::wstring& key,
               const std::wstring& valuename, const std::wstring& data) override;
    bool keyExists(const std::wstring& key, bool* result) override;
    bool valueExists(const std::wstring& key, const std::wstring& valuename, bool* result) override;
    bool readValue(const std::wstring& key, const std::wstring& valuename,
                   std::wstring* result) override;
    bool keyEmpty(const std::wstring& key, bool* result) override;

    const std::vector<RegistryOperation>& operations() const { return operations_; }

private:
    bool deleted(const std::wstring& normalized) const;
    bool really_exists(const std::wstring& key) const;
    // Whether the key would be refused to an elevated process: its nearest
    // existing ancestor denies Administrators subkey creation, and this dry run
    // has not taken ownership of it and made it writable.
    bool create_would_be_denied(const std::wstring& key) const;
    std::set<std::wstring> made_writable_;

    std::vector<RegistryOperation> operations_;
    std::set<std::wstring> created_;
    std::set<std::wstring> deleted_;
    // normalized key + L'|' + lower-case value name -> data; absent from the
    // map means "ask the registry", a present entry with `removed` means gone.
    struct Value { std::wstring data; bool removed = false; };
    std::map<std::wstring, Value> values_;
};

// Records the writes a real run made, in order. Only writes that succeeded are
// listed, so after a failure it is exactly what changed.
class OperationLog : public RegistryLog {
public:
    void write(const std::wstring& operation, const std::wstring& key,
               const std::wstring& valuename, const std::wstring& data) override {
        operations_.push_back({operation, key, valuename, data});
    }
    const std::vector<RegistryOperation>& operations() const { return operations_; }

private:
    std::vector<RegistryOperation> operations_;
};

// While alive, reports RegistryHelper's real writes to `log`.
class ScopedLog {
public:
    explicit ScopedLog(RegistryLog* log) { RegistryHelper::log = log; }
    ~ScopedLog() { RegistryHelper::log = nullptr; }
    ScopedLog(const ScopedLog&) = delete;
    ScopedLog& operator=(const ScopedLog&) = delete;
};

// While alive, routes RegistryHelper through `registry`.
class ScopedDryRun {
public:
    explicit ScopedDryRun(RegistryDryRun* registry) { RegistryHelper::dryRun = registry; }
    ~ScopedDryRun() { RegistryHelper::dryRun = nullptr; }
    ScopedDryRun(const ScopedDryRun&) = delete;
    ScopedDryRun& operator=(const ScopedDryRun&) = delete;
};

}  // namespace isotone::devicetool
