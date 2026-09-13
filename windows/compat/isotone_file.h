// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Isotone.txt: the file Equalizer APO reads Isotone's per-device state from
// (plan 5.5). One block per device, in the same order as the processor's
// stages:
//
//   Device: {endpoint-guid}
//   Channel: all
//   # Isotone: speakers delay_ms=... ...  the SpeakerSetup, when not the default
//   # Isotone: routing                    swaps, upmix, bass management
//   Copy: L=1*R R=1*L
//   Copy: ISOTONEBASS=1*L+1*R
//   Channel: LFE
//   Filter: ON LPQ Fc 120 Hz Q 0.707106781187      (twice: 24 dB/oct)
//   ...
//   Copy: LFE=1*LFE+1*ISOTONEBASS
//   # Isotone: end
//   Preamp: -6 dB                         format_apo_config for the device layout
//   Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1
//   ...
//   # Isotone: output                     polarity, speaker mute, delay
//   Channel: all
//   Copy: C=-1*C SL=0
//   Channel: L R
//   Delay: 3.5 ms
//   # Isotone: end
//   # Isotone: mute                       only when muted
//   Channel: all
//   Preamp: -100 dB
//
// Why each piece is there, from upstream's source (pinned commit in
// windows/devicetool/upstream/VENDORED.md):
// - `Device: {guid}` is the narrowest match DeviceFilterFactory::matchDevice
//   allows (measured in decisions.md, stage 1a).
// - `Channel: all` because an included file starts with whatever channel
//   selection the including file had at its Include line
//   (FilterEngine::loadConfigFile saves and restores it around the include).
// - Copy reads every channel before writing any (CopyFilter is not in place),
//   so `L=1*R R=1*L` swaps. Channels it does not assign keep their samples
//   (FilterConfiguration::process swaps buffers only for targets). A target
//   name that is not a device channel creates a virtual channel, zeroed each
//   block and never output, which is how ISOTONEBASS collects the bass.
// - Delay rounds to whole samples, rate * ms / 1000 + 0.5, as the processor does.
// - Mute is a gain stage on all channels rather than the per-channel trims
//   format_apo_config writes, which only reach the first kMaxChannels.
// - Bypass keeps the block but comments every line after `# Isotone: bypass`,
//   so the file still carries the settings and upstream applies none of them.
//
// `# Isotone:` lines are comments to upstream (the key "# Isotone" matches no
// filter factory) and markers to parse_isotone_file.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/types.h"

namespace isotone::compat {

struct DeviceConfig {
    std::string   endpoint_guid;   // with or without braces
    ChannelLayout layout;          // the device's mix format: channel count and speaker mask
    EqState       state;
};

// The block for one device, ending in a newline.
std::string format_device_block(const DeviceConfig& device);

// Replaces the block for `device` in an existing Isotone.txt, or appends one.
// Every other byte of `existing`, other devices' blocks included, is kept.
std::string update_isotone_file(const std::string& existing, const DeviceConfig& device);

// Removes a device's block; returns `existing` unchanged if there is none.
std::string remove_device(const std::string& existing, const std::string& endpoint_guid);

struct ParsedDevice {
    std::string                  endpoint_guid;   // as written in the Device line
    EqState                      state;
    std::vector<ApoParseMessage> warnings;
    std::vector<std::string>     unsupported;
};

// Reads Isotone.txt back into per-device state. Bands, preamp and trims come
// from parse_apo_config with that device's layout; bypass, mute and the speaker
// setup from the markers. Band ids are not stored in the file, and disabled
// bands are written as OFF, which upstream and the parser skip, so neither
// round-trips.
std::vector<ParsedDevice> parse_isotone_file(
    const std::string& text,
    const std::function<ChannelLayout(const std::string& endpoint_guid)>& layout_for);


}  // namespace isotone::compat
