// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Isotone.txt: the file Equalizer APO reads Isotone's per-device state from
// (plan 5.5). One block per device, in the same order as the processor's
// stages. This one is format_device_block's output for a 5.1 device (0x60F) at
// 48 kHz with a -6 dB preamp, one band, a trim on L, left and right swapped,
// bass management for L R SL SR, C inverted, SR muted, 3.5 ms lip sync, and
// mute on:
//
//   Device: {798436D2-8C71-4834-9248-00CCBAACA00A}
//   Channel: all
//   # Isotone: speakers delay_ms=0,0,0,0,0,0,0,0 lip_sync_ms=3.5 ...   the SpeakerSetup, when not the default
//   # Isotone: layout 6 0x60f                       the layout its per-speaker values are for
//   # Isotone: routing                              swaps, upmix, bass management
//   If: outputChannelCount == 6
//   Copy: 1=1*2 2=1*1 5=1*6 6=1*5
//   Copy: ISOTONEBASS=1*1+1*2+1*5+1*6
//   Channel: 4
//   Filter: ON LPQ Fc 120 Hz Q 0.707106781187       twice: 24 dB/oct
//   Filter: ON LPQ Fc 120 Hz Q 0.707106781187
//   Channel: ISOTONEBASS
//   Filter: ON LPQ Fc 80 Hz Q 0.707106781187
//   Filter: ON LPQ Fc 80 Hz Q 0.707106781187
//   Channel: 1 2 5 6
//   Filter: ON HPQ Fc 80 Hz Q 0.707106781187
//   Filter: ON HPQ Fc 80 Hz Q 0.707106781187
//   Channel: all
//   Copy: 4=1*4+1*ISOTONEBASS
//   EndIf:
//   Channel: all
//   # Isotone: end
//   Preamp: -6 dB                                   format_apo_config for the layout
//   If: sampleRate >= 2106                          the bands only
//   Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1
//   EndIf:
//   Channel: L                                      trims
//   Preamp: -1.5 dB
//   # Isotone: output                               polarity, speaker mute, delay, by name
//   Channel: all
//   Copy: C=-1*C
//   Channel: SR
//   Preamp: -1000 dB
//   Channel: all                                    lip sync, then each speaker's own delay
//   Delay: 3.5 ms
//   Channel: all
//   # Isotone: end
//   # Isotone: mute                                 only when muted
//   Channel: all
//   Preamp: -1000 dB
//
// Bypassed, the preamp and the bands, with their rate guard, become comments
// between markers, and everything else stays:
//
//   # Isotone: bypass
//   # Preamp: -6 dB
//   # If: sampleRate >= 2106
//   # Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1
//   # EndIf:
//   # Isotone: end
//
// Why each piece is there, from upstream's source (pinned commit in
// windows/devicetool/upstream/VENDORED.md):
// - `Device: {guid}` is the narrowest match DeviceFilterFactory::matchDevice
//   allows (measured in decisions.md, stage 1a).
// - `Channel: all` because an included file starts with whatever channel
//   selection the including file had at its Include line
//   (FilterEngine::loadConfigFile saves and restores it around the include).
// - Routing names channels by number inside `If: outputChannelCount == N`.
//   Equalizer APO applies the file to whatever format the device has when it
//   loads, and a Copy source naming a channel that layout lacks is added as a
//   constant (CopyFilter), which is DC. Under the guard a number always resolves.
// - Output names channels, as the bands and trims do, so each value follows its
//   speaker to another layout, as remap_channels moves IsoAPO's. Delay and
//   Preamp act on the Channel selection, which is empty for a name the layout
//   lacks (ChannelFilter). Polarity's Copy has a channel as both target and
//   source, which resolve against the same names (CopyFilter::initialize): a
//   name the layout lacks makes a virtual channel for the target, and the
//   constant its source is read as goes there, not to an output.
// - Copy reads every channel before writing any (CopyFilter is not in place),
//   so `1=1*2 2=1*1` swaps. Channels it does not assign keep their samples
//   (FilterConfiguration::process swaps buffers only for targets). A target
//   name that is not a device channel creates a virtual channel, zeroed each
//   block and never output, which is how ISOTONEBASS collects the bass.
// - The bands are guarded by `If: sampleRate >= X`, the lowest rate at which
//   every band is written below the processor's clamp: upstream designs a band
//   above Nyquist unstable. The preamp is outside the guard, as the processor
//   keeps it at every rate and routing can sum channels into one.
// - Delay rounds to whole samples, rate * ms / 1000 + 0.5, as the processor does.
//   The processor rounds lip sync plus a speaker's delay once and upstream
//   rounds each Delay line, so each line is a whole number of samples at the
//   device's rate.
// - Mute is a -1000 dB preamp on all channels. Upstream stores a preamp as a
//   float gain, and 10^(-1000/20) is below the smallest float: exact silence on
//   any layout, channels a grown layout added included.
//
// `# Isotone:` lines and `# ` comments are comments to upstream (their keys
// match no filter factory) and markers to parse_isotone_file.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/types.h"

namespace isotone::compat {

struct DeviceConfig {
    std::string   endpoint_guid;   // with or without braces, or a full device ID
    // The device's mix format: channel count and speaker mask. 0 channels is
    // unset, and CompatWriter refuses it.
    ChannelLayout layout{0, 0};
    double        sample_rate = 0; // the device's rate; 0 is unset, and CompatWriter refuses it
    EqState       state;
};

// The block for one device, ending in a newline. Per-channel values in `state`
// written for another layout (EqState::layout_channels) are first moved to
// `layout` by remap_channels. Routing is written for `layout`'s channel count
// and does nothing on another; bands, trims, polarity, speaker mute and delay
// follow their speaker by name, as remap_channels does; bands are
// written for `sample_rate` and do nothing at a rate where they would be
// unstable, and a rate of 0 writes them unclamped and unguarded. Bypass comments
// out the preamp and bands only.
std::string format_device_block(const DeviceConfig& device);

// Replaces the block for `device` in an existing Isotone.txt, or appends one.
// Every other byte of `existing`, other devices' blocks included, is kept,
// except further blocks for the same device, which are removed.
std::string update_isotone_file(const std::string& existing, const DeviceConfig& device);

// Removes a device's blocks; returns `existing` unchanged if there are none.
std::string remove_device(const std::string& existing, const std::string& endpoint_guid);

struct ParsedDevice {
    std::string                  endpoint_guid;   // as written in the Device line
    EqState                      state;
    std::vector<ApoParseMessage> warnings;
    std::vector<std::string>     unsupported;
};

// Reads Isotone.txt back into per-device state, for the layout `layout_for`
// gives (stereo when it gives none), which the state records. Bands, preamp
// and trims come from parse_apo_config with that layout; bypass, mute and the
// speaker setup from the markers, with the per-speaker values moved from the
// layout they were written for. Band ids are not stored in the file, so they do
// not round-trip; disabled bands are written as OFF lines, which upstream skips
// and the parser reads back as disabled bands.
std::vector<ParsedDevice> parse_isotone_file(
    const std::string& text,
    const std::function<ChannelLayout(const std::string& endpoint_guid)>& layout_for);


}  // namespace isotone::compat
