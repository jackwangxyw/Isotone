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

// A device's channel layout: how many channels, and which Windows speaker
// position each carries (the WAVEFORMATEXTENSIBLE dwChannelMask, in bit order).
// Equalizer APO resolves channel names through it (helpers/ChannelHelper.cpp),
// so `SL` is channel 5 on 5.1 but channel 7 on 7.1. Pass the layout of the
// device a config is for. The default is 7.1 surround.
struct ChannelLayout {
    uint32_t channels     = 8;
    uint32_t speaker_mask = 0x63F;   // KSAUDIO_SPEAKER_7POINT1_SURROUND
};

// Upstream's mask for a stream that reports none (getDefaultChannelMask):
// mono, stereo, quad, 5.1 surround and 7.1 surround; 0 for any other count.
uint32_t default_speaker_mask(uint32_t channels);

// Upstream's name for each channel of a layout (getChannelNames): L R C LFE RL
// RR RC SL SR for those positions, and the 1-based channel number for any
// other position or for channels the mask does not cover. On input, `SUB` is an
// alias for LFE, and SL/SR and RL/RR stand in for each other when the layout
// has only one pair.
std::vector<std::string> apo_channel_names(const ChannelLayout& layout);

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
ApoParseResult parse_apo_config(const std::string& text, const ChannelLayout& layout = {});

struct ApoFormatOptions {
    // Emitted as a `Device:` line before anything else. A full endpoint GUID in
    // braces is the narrowest possible match and cannot collide with another
    // device. Empty means no Device line at all.
    std::string device;

    // Channel names in Channel: lines are written for this layout.
    ChannelLayout layout;

    // Written above the filters. Each line is prefixed with '# '.
    std::string header_comment;

    // The rate of the device the text is for, or 0 for an export. With a rate,
    // each frequency is written as the processor designs it (clamp_fc):
    // upstream does not clamp, and a band above Nyquist makes its biquad
    // unstable, which Equalizer APO plays as silence.
    double sample_rate = 0.0;

    // Equalizer APO numbers filters from 1 and Peace leaves gaps; we always
    // write a dense sequence.
    bool write_disabled_as_none = true;
};

std::string format_apo_config(const EqState& state, const ApoFormatOptions& options = {});

// A number as the config format writes it: up to 12 significant digits, with a
// period, whatever the C locale.
std::string format_apo_number(double v);

// A frequency in Hz as it has to be written for Equalizer APO. Upstream reads a
// value like "80.125" as Room EQ Wizard's thousands separator and multiplies it
// by 1000; this adds a trailing zero to any value that would match that rule.
std::string format_apo_frequency(double hz);

// Reads the leading number of `s` with a period as the decimal mark, whatever
// the C locale, as upstream's wcstod-based parsing does. A leading '+' is
// allowed. False if there is no number or it is not finite.
bool parse_apo_number(const std::string& s, double* out);

// Escapes nothing and quotes nothing: Equalizer APO's Device patterns are plain
// substring matches. Returns a pattern that matches only the given endpoint.
std::string apo_device_pattern_for_guid(const std::string& guid);

}  // namespace isotone
