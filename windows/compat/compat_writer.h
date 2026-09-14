// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The compatibility backend's transport for one Equalizer APO config
// directory: keeps Isotone.txt in memory, applies a device's state to it, and
// writes it atomically when an edit is committed.
//
// This is the piece an app-side EqBackend (plan 5.5) calls for apply() and
// persist(). Attaching the include to config.txt is a separate, consented step
// (config_files.h).
//
// Live edits are not written. Every write Equalizer APO notices in its config
// directory makes every Equalizer APO instance on the machine rebuild its
// filter chain from rest, with a 10 ms crossfade (measured on the VB-Cable
// pair): a delay line restarts from zero, so a channel delayed by D is silent
// for about D - 10 ms; a high-Q bass band starts from rest and swells (40 Hz,
// Q 10, -12 dB played up to +10.5 dB loud, over 1 dB for about 130 ms); the
// high-pass of any other configuration on any device restarts too. At 10 or 30
// writes a second a delayed channel stays silent and a bass band never settles
// for the whole drag. So apply() only updates the text, and the audio follows
// on persist() (the UI calls it when the drag ends), flush() or destruction.
//
// Several writers can share a directory (the UI and isotone-compat apply, in
// different processes and possibly for different Windows users). Each write
// takes a lock (an exclusive open of Isotone.txt.lock), re-reads the file, and
// if another writer changed it, puts this writer's devices onto what is there;
// it replaces the file only if the result differs from what is on disk. So
// neither writer reverts the other's blocks, and a block another writer changed
// is written back when this writer next writes its state.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "isotone_file.h"

namespace isotone::compat {

class CompatWriter {
public:
    explicit CompatWriter(std::filesystem::path config_dir);
    CompatWriter(const CompatWriter&) = delete;
    CompatWriter& operator=(const CompatWriter&) = delete;
    // Writes a live edit still pending, and cannot report failure: call flush()
    // first to know whether the last edit reached the file.
    ~CompatWriter();

    // Reads the current Isotone.txt so other devices' blocks survive the next
    // write. A missing file is an empty one; a missing directory is an error.
    DWORD load();

    // Live edit: updates text() and leaves the write pending; nothing on disk
    // changes. ERROR_INVALID_PARAMETER, with nothing changed, when the device's
    // channel count (layout.channels) or sample rate is 0, or the count is more
    // than kMaxApoChannels: the block depends on both.
    DWORD apply(const DeviceConfig& device);

    // Written now, along with any pending live edit. Refuses as apply() does.
    // A failed write stays pending.
    DWORD persist(const DeviceConfig& device);
    DWORD remove(const std::string& endpoint_guid);

    // Writes a pending live edit now; a failed write stays pending.
    DWORD flush();
    bool has_pending() const { return pending_; }

    const std::string& text() const { return text_; }
    const std::filesystem::path& path() const { return path_; }

private:
    DWORD write();

    std::filesystem::path dir_;
    std::filesystem::path path_;
    std::string text_;
    std::string on_disk_;   // the file as this writer last read or wrote it
    // The devices this writer has written or removed, latest state, by GUID key.
    std::vector<std::pair<std::string, std::optional<DeviceConfig>>> mine_;
    bool pending_ = false;   // text_ has changes not yet written
};

}  // namespace isotone::compat
