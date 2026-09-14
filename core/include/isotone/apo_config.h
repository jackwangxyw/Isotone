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

// The most channels a stream can have, and so upstream: WAVEFORMATEX counts
// them in 16 bits. A layout with more is read as having this many.
inline constexpr uint32_t kMaxApoChannels = 65535;

// Upstream's mask for a stream that reports none (getDefaultChannelMask):
// mono, stereo, quad, 5.1 surround and 7.1 surround; 0 for any other count.
uint32_t default_speaker_mask(uint32_t channels);

// Upstream's name for each channel of a layout (getChannelNames): L R C LFE RL
// RR RC SL SR for those positions, and the 1-based channel number for any
// other position or for channels the mask does not cover. A mask of 0 is
// replaced by default_speaker_mask first, as upstream's FilterEngine does. On input, `SUB` is an
// alias for LFE, and SL/SR and RL/RR stand in for each other when the layout
// has only one pair.
std::vector<std::string> apo_channel_names(const ChannelLayout& layout);

// Moves the per-channel values of `state` (see EqState::layout_channels) from
// the layout they were written for to `layout`, the way Equalizer APO resolves
// a channel name written for one layout on another: each channel is named as
// apo_channel_names names it on the old layout, and that name is looked up on
// `layout` as a Channel line's word is. So a speaker keeps its values wherever
// it sits in the new layout, SL and RL (SR and RR) stand in for each other when
// the new layout has only one of them, and a numbered channel keeps its index.
// Values of a channel that resolves to nothing, or to a channel past the new
// count, are dropped. Values that land on one channel combine as the Equalizer
// APO backend's lines for them do: band channels, polarity, speaker mute and
// small speakers as a union, trims and speaker delays as a sum. A band left with
// no channel is disabled, keeping its mask, since an empty mask would mean all
// channels; a band on all channels stays on all. The state's layout becomes
// `layout`.
//
// Nothing changes when the state's layout is unspecified (0 channels), when
// `layout` has 0 channels, or when the two are the same once a mask of 0 is
// read as the default for its count. Real-time safe: no allocation.
void remap_channels(EqState* state, const ChannelLayout& layout);

struct ApoParseMessage {
    size_t      line = 0;      // 1-based
    std::string text;
};

struct ApoParseResult {
    EqState state;

    // Lines the importer understood but cannot represent: convolution, VST,
    // GraphicEQ, Delay, Copy, Include, If/Else. Kept verbatim so an exporter
    // can write them back out and a user's config is not silently destroyed on
    // a round trip. Lines under a Stage that is not for playback are skipped.
    std::vector<std::string> unsupported;

    // Device: patterns seen, in order. Empty means the file was unscoped.
    std::vector<std::string> devices;

    // Every line that was not applied as written, `unsupported` lines included.
    std::vector<ApoParseMessage> warnings;

    bool ok() const { return warnings.empty(); }
};

// Parses an Equalizer APO configuration. Never throws and never fails outright:
// unparseable lines become warnings, exactly as upstream logs and skips them.
// Filter lines are read with upstream's own patterns, so a line it ignores
// (lower-case `on`, a frequency without `Hz`) is ignored here too, with a
// warning. A result with warnings may not measure as the file does in
// Equalizer APO: Device sections for particular devices and If/Else branches
// are all imported, since only the device the file runs on can decide them.
// A filter line with OFF and a whole filter after it is read as a disabled band.
//
// `layout` must be the layout of the device the result is for. Channel names
// resolve through it, and mute is recognised only as a Copy line silencing
// every channel it has: mute written for stereo and read as 7.1 is not mute,
// as it is not on a 7.1 device in Equalizer APO.
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
