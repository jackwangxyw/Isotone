// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The compatibility backend's transport for one Equalizer APO config
// directory: keeps Isotone.txt in memory, applies a device's state to it, and
// writes it atomically, rate-limited for live edits.
//
// This is the piece an app-side EqBackend (plan 5.5) calls for apply() and
// persist(). Attaching the include to config.txt is a separate, consented step
// (config_files.h).

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "isotone_file.h"
#include "write_coalescer.h"

namespace isotone::compat {

class CompatWriter {
public:
    explicit CompatWriter(std::filesystem::path config_dir, WriteCoalescer::Clock clock = nullptr);
    CompatWriter(const CompatWriter&) = delete;              // the write sink captures `this`
    CompatWriter& operator=(const CompatWriter&) = delete;

    // Reads the current Isotone.txt so other devices' blocks survive the next
    // write. A missing file is an empty one; a missing directory is an error.
    DWORD load();

    // Live edit: coalesced to about 30 writes a second. Call poll() from a timer.
    DWORD apply(const DeviceConfig& device);

    // Written now, along with any pending live edit.
    DWORD persist(const DeviceConfig& device);
    DWORD remove(const std::string& endpoint_guid);

    DWORD poll() { return coalescer_.poll(); }
    bool has_pending() const { return coalescer_.has_pending(); }
    std::chrono::milliseconds time_until_due() const { return coalescer_.time_until_due(); }
    size_t writes() const { return coalescer_.writes(); }

    const std::string& text() const { return text_; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
    std::string text_;
    WriteCoalescer coalescer_;
};

}  // namespace isotone::compat
