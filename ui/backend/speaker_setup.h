// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The speaker controls of outputs with more than two channels (docs/ui-spec.md,
// "Speakers panel", "Speakers view", "Bass management"): the speakers a layout
// has, speaker groups as channel masks, a band's target label, solo, what test
// tones change in the state the engine gets, distance and delay, the saved
// state's speaker part, and which channel the graph draws.
//
// Channel n of a layout is the n-th set bit of its speaker mask (speakers.h).
// Masks here are over those channels; a band mask of 0 is every channel.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "isotone/types.h"

namespace isotone::ui {

struct Speaker {
    uint32_t channel = 0;
    uint32_t bit = 0;     // the speaker position (SPEAKER_*), 0 for a channel with no named position
    std::string code;     // "L", "LFE", "SL"; the channel number for a position without a name
    std::string name;     // "Front left", "Subwoofer"
};

// The speakers of a layout in channel order. A mask of 0 is the default for the
// channel count (default_speaker_mask). At most kMaskChannels channels.
std::vector<Speaker> layout_speakers(uint32_t channels, uint32_t speaker_mask);

// Every channel of the layout as a mask (not 0: that is "all" only on a band).
ChannelMask layout_channel_mask(uint32_t channels);

// A group the user made, by speaker code, so it keeps its speakers when the
// layout changes.
struct SpeakerGroup {
    std::string name;
    std::vector<std::string> codes;
};

struct ResolvedGroup {
    std::string name;
    ChannelMask mask = 0;   // over the layout's channels
    bool builtin = false;
};

// The mask of `codes` on the layout; codes it does not have add nothing.
ChannelMask codes_mask(const std::vector<std::string>& codes, uint32_t channels, uint32_t speaker_mask);

// All, Front (L C R), Surround (SL SR RL RR), Sub (LFE), then the user's groups,
// each with the speakers the layout has. A group with none of them is left out.
std::vector<ResolvedGroup> layout_groups(uint32_t channels, uint32_t speaker_mask, const std::vector<SpeakerGroup>& user);

// What a band's target column shows on a surround output: "All" for every
// channel, a group's name when the mask is exactly that group (built-in groups
// first), a speaker's name when it is one speaker, else the speakers' codes
// ("L SL"); "None" when the mask has none of the layout's channels.
std::string target_label(ChannelMask band_channels, uint32_t channels, uint32_t speaker_mask,
                         const std::vector<SpeakerGroup>& user);

// The speakers solo mutes: every other speaker of the layout, except the LFE when
// the soloed speaker is small (bass management on and the speaker in
// small_speakers), so its redirected bass still plays. 0 when `solo_channel` is
// not one of the layout's channels.
ChannelMask solo_mute_mask(const SpeakerSetup& setup, uint32_t channels, uint32_t speaker_mask, int solo_channel);

// What the engine gets on top of the edited state and never saved.
struct LiveOverrides {
    ChannelMask solo_muted = 0;   // solo_mute_mask
    bool test_tones = false;      // bypass (bands and preamp), upmix and both swaps off
};
void apply_live_overrides(const LiveOverrides& overrides, EqState* state);

// Distance and delay (docs/ui-spec.md, "Speakers view"): a speaker's delay is
// (farthest - its distance) / 343 m/s. The engine keeps only delays, so the
// distances are the delays read back from the farthest speaker's distance.
inline constexpr double kSpeedOfSoundMps = 343.0;
inline constexpr double kDefaultFarthestM = 3.0;
inline constexpr double kMaxDistanceM = 50.0;
inline constexpr double kMaxSpeakerDelayMs = 1000.0;
double delay_ms_for_distance(double farthest_m, double distance_m);
double distance_for_delay(double farthest_m, double delay_ms);
// Sets `channel`'s distance: every delay is recomputed from the distances, and
// `farthest_m` becomes the largest of them.
void set_speaker_distance(SpeakerSetup* setup, uint32_t channels, double* farthest_m, uint32_t channel,
                          double distance_m);
// Sets `channel`'s delay; `farthest_m` grows when the delay puts the speaker
// nearer than 0 m.
void set_speaker_delay(SpeakerSetup* setup, double* farthest_m, uint32_t channel, double delay_ms);

// Ranges and steps. Bass management follows AV receivers (ui-spec.md).
inline constexpr double kCrossoverMinHz = 40.0, kCrossoverMaxHz = 250.0;
inline constexpr double kLfeLowpassMinHz = 80.0, kLfeLowpassMaxHz = 250.0;
inline constexpr double kBassStepHz = 10.0;
inline constexpr double kLevelMinDb = -24.0, kLevelMaxDb = 12.0;
inline constexpr double kLipSyncMaxMs = 500.0;
double snap_crossover_hz(double hz);
double snap_lfe_lowpass_hz(double hz);

// The saved state after a speaker setup change: `saved` (flat when null) with
// the speaker setup, levels and layout of `live`. The bands, preamp and mute
// stay the saved ones, so an unsaved edit of the bands is not saved with it.
EqState saved_with_speakers(const EqState* saved, const EqState& live);

// Reads the saved state at `path`, replaces its speaker part with `live`'s and
// writes it back (file only). A missing or invalid file counts as flat. A Win32
// error code on Windows, an errno on Linux.
unsigned long save_speaker_setup(const std::filesystem::path& path, const EqState& live);

// The graph on a surround output. `view` is the Showing picker's mask, 0 for
// every speaker. The composite is drawn for the channel in view with the most
// bands on it (the first on a tie); a band's handle sits on that channel when
// the band is on it, else on the first channel in view the band is on.
bool band_in_view(const Band& band, uint32_t channels, ChannelMask view);
uint32_t primary_view_channel(const EqState& state, uint32_t channels, ChannelMask view);
uint32_t band_view_channel(const Band& band, uint32_t channels, ChannelMask view, uint32_t primary);

}  // namespace isotone::ui
