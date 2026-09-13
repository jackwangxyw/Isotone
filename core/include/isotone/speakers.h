// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Speaker positions in a multichannel stream. Windows describes a stream's
// layout with a speaker mask (WAVEFORMATEXTENSIBLE::dwChannelMask); channel n is
// the n-th set bit, lowest first. PipeWire's positions map onto the same bits.

#pragma once

#include <cstdint>
#include <string>

#include "isotone/types.h"

namespace isotone {

// ksmedia.h SPEAKER_* bits.
inline constexpr uint32_t kSpeakerFrontLeft   = 0x1;
inline constexpr uint32_t kSpeakerFrontRight  = 0x2;
inline constexpr uint32_t kSpeakerFrontCenter = 0x4;
inline constexpr uint32_t kSpeakerLowFrequency = 0x8;
inline constexpr uint32_t kSpeakerBackLeft    = 0x10;
inline constexpr uint32_t kSpeakerBackRight   = 0x20;
inline constexpr uint32_t kSpeakerSideLeft    = 0x200;
inline constexpr uint32_t kSpeakerSideRight   = 0x400;

// Channel index of `speaker_bit` in a stream with `speaker_mask` and `channels`
// channels, or -1 when the stream has no such speaker.
inline int speaker_channel(uint32_t speaker_mask, uint32_t channels, uint32_t speaker_bit) {
    if ((speaker_mask & speaker_bit) == 0) {
        return -1;
    }
    int index = 0;
    for (uint32_t bit = 1; bit < speaker_bit; bit <<= 1) {
        if ((speaker_mask & bit) != 0) {
            ++index;
        }
    }
    return static_cast<uint32_t>(index) < channels ? index : -1;
}

// The routing SpeakerSetup asks for (upmix, then the swaps) as a matrix over
// the first min(channels, kMaxChannels) channels: out[o] = sum m[o][i] * in[i].
// Entries outside that range are identity. The processor and the Equalizer APO
// backend both build their routing from this, so they cannot disagree.
void routing_matrix(const SpeakerSetup& setup, uint32_t speaker_mask, uint32_t channels,
                    double m[kMaxChannels][kMaxChannels]);

// Total delay of `channel` in milliseconds: its speaker delay plus lip sync,
// each clamped to [0, max_ms] and the sum clamped to max_ms. Non-finite values
// count as 0.
double channel_delay_ms(const SpeakerSetup& setup, uint32_t channel, double max_ms);

// A SpeakerSetup as space-separated key=value pairs, the form Isotone.txt's
// speakers marker and the tools' --speakers take:
//   delay_ms=0,0,1.3,0,0,0,0,0 lip_sync_ms=0 inverted=0x0 muted=0x0
//   swap_left_right=0 swap_front_rear=0 upmix=off bass_management=0
//   crossover_hz=80 small_speakers=0x0 lfe_lowpass_hz=120
std::string format_speaker_setup(const SpeakerSetup& setup);

// Parses that form. Keys may be omitted (they keep the default) but not
// unknown or malformed; on false, `error` says which.
bool parse_speaker_setup(const std::string& text, SpeakerSetup* setup, std::string* error);

}  // namespace isotone
