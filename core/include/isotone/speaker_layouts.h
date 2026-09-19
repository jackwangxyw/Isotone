// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The layouts the Speakers view's picker offers: Stereo, 2.1, 5.1 and 7.1, with
// their channel counts and speaker masks. Portable, so the UI names a layout the
// same way on both platforms; windows/devices/speaker_layout.h is what changes an
// endpoint's format to one of them on Windows.

#pragma once

#include <cstdint>
#include <string_view>

namespace isotone {

enum class SpeakerLayout { stereo, two_point_one, five_point_one, seven_point_one };

struct SpeakerLayoutSpec {
    SpeakerLayout layout;
    uint16_t channels;
    uint32_t mask;        // the speaker bits of isotone/speakers.h, which are ksmedia.h's KSAUDIO_SPEAKER_*
    const char* name;     // "stereo", "2.1", "5.1", "7.1"
};

// In picker order; kSpeakerLayouts[i].layout == SpeakerLayout(i).
//   stereo  2  0x3    KSAUDIO_SPEAKER_STEREO            FL FR
//   2.1     3  0xB    KSAUDIO_SPEAKER_2POINT1           FL FR LFE
//   5.1     6  0x60F  KSAUDIO_SPEAKER_5POINT1_SURROUND  FL FR FC LFE SL SR
//   7.1     8  0x63F  KSAUDIO_SPEAKER_7POINT1_SURROUND  FL FR FC LFE BL BR SL SR
// 5.1 and 7.1 are the masks isotone::default_speaker_mask gives 6 and 8
// channels. It gives 3 channels no mask, so 2.1 exists only with its mask.
inline constexpr SpeakerLayoutSpec kSpeakerLayouts[] = {
    {SpeakerLayout::stereo, 2, 0x3, "stereo"},
    {SpeakerLayout::two_point_one, 3, 0xB, "2.1"},
    {SpeakerLayout::five_point_one, 6, 0x60F, "5.1"},
    {SpeakerLayout::seven_point_one, 8, 0x63F, "7.1"},
};

// Null for a value outside the enum.
inline const SpeakerLayoutSpec* speaker_layout_spec(SpeakerLayout layout) {
    for (const SpeakerLayoutSpec& spec : kSpeakerLayouts) {
        if (spec.layout == layout) return &spec;
    }
    return nullptr;
}

// The spec's name, case-sensitive. False for anything else.
inline bool parse_speaker_layout(std::string_view name, SpeakerLayout* out) {
    for (const SpeakerLayoutSpec& spec : kSpeakerLayouts) {
        if (name == spec.name) {
            *out = spec.layout;
            return true;
        }
    }
    return false;
}

}  // namespace isotone
