// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Equalizer APO config.txt: reading and writing.
//
// This is both the import/export format for presets (AutoEq publishes in it,
// Room EQ Wizard exports it, Peace writes it) and the transport for the
// compatibility backend, which drives a stock Equalizer APO install by writing
// files it reads.
//
// Behaviour is matched to upstream rather than to any specification, because the
// requirement is that a file imported here measures the same as it does there.
// The quirks below are upstream's, verified by reading its source.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "isotone/types.h"

namespace isotone {

// Channel names as Equalizer APO spells them, indexed by channel number in the
// usual Windows order. `SUB` is accepted on input as an old alias for LFE, and
// SL/SR fall back to RL/RR when a device has no side channels.
const char* apo_channel_name(uint32_t channel);
bool apo_channel_index(const std::string& name, uint32_t* out);

struct ApoParseMessage {
    size_t      line = 0;      // 1-based
    std::string text;
};

struct ApoParseResult {
    EqState state;

    // Lines the importer understood but cannot represent: convolution, VST,
    // GraphicEQ, Delay, Copy, Include, If/Stage expressions. Kept verbatim so an
    // exporter can write them back out and a user's config is not silently
    // destroyed on a round trip.
    std::vector<std::string> unsupported;

    // Device: patterns seen, in order. Empty means the file was unscoped.
    std::vector<std::string> devices;

    std::vector<ApoParseMessage> warnings;

    bool ok() const { return warnings.empty(); }
};

// Parses an Equalizer APO configuration. Never throws and never fails outright:
// unparseable lines become warnings, exactly as upstream logs and skips them.
ApoParseResult parse_apo_config(const std::string& text);

struct ApoFormatOptions {
    // Emitted as a `Device:` line before anything else. A full endpoint GUID in
    // braces is the narrowest possible match and cannot collide with another
    // device. Empty means no Device line at all.
    std::string device;

    // Written above the filters. Each line is prefixed with '# '.
    std::string header_comment;

    // Equalizer APO numbers filters from 1 and Peace leaves gaps; we always
    // write a dense sequence.
    bool write_disabled_as_none = true;
};

std::string format_apo_config(const EqState& state, const ApoFormatOptions& options = {});

// Escapes nothing and quotes nothing: Equalizer APO's Device patterns are plain
// substring matches. Returns a pattern that matches only the given endpoint.
std::string apo_device_pattern_for_guid(const std::string& guid);

}  // namespace isotone
