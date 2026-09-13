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
//
// Several writers can share a directory (the UI and isotone-compat apply). Each
// write re-reads the file first, and if another writer changed it, puts this
// writer's devices onto what is there, so neither reverts the other's blocks.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "isotone_file.h"
#include "write_coalescer.h"

namespace isotone::compat {

class CompatWriter {
public:
    explicit CompatWriter(std::filesystem::path config_dir, WriteCoalescer::Clock clock = nullptr);
    CompatWriter(const CompatWriter&) = delete;              // the write sink captures `this`
    CompatWriter& operator=(const CompatWriter&) = delete;
    // Writes a live edit still pending.
    ~CompatWriter();

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
    DWORD write(const std::string& bytes);

    std::filesystem::path dir_;
    std::filesystem::path path_;
    std::string text_;
    std::string on_disk_;   // the file as this writer last read or wrote it
    // The devices this writer has written or removed, latest state, by GUID key.
    std::vector<std::pair<std::string, std::optional<DeviceConfig>>> mine_;
    WriteCoalescer coalescer_;
};

}  // namespace isotone::compat
