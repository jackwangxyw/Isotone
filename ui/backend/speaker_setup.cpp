// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "speaker_setup.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "isotone/speakers.h"
#include "persisted_state.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#endif

namespace isotone::ui {

namespace {

struct Position {
    uint32_t bit;
    const char* code;
    const char* name;
};
constexpr Position kPositions[] = {
    {kSpeakerFrontLeft, "L", "Front left"},   {kSpeakerFrontRight, "R", "Front right"},
    {kSpeakerFrontCenter, "C", "Centre"},     {kSpeakerLowFrequency, "LFE", "Subwoofer"},
    {kSpeakerBackLeft, "RL", "Rear left"},    {kSpeakerBackRight, "RR", "Rear right"},
    {0x100, "RC", "Rear centre"},             {kSpeakerSideLeft, "SL", "Side left"},
    {kSpeakerSideRight, "SR", "Side right"},
};

uint32_t effective_mask(uint32_t channels, uint32_t speaker_mask) {
    return speaker_mask != 0 ? speaker_mask : default_speaker_mask(channels);
}

bool finite(double v) { return std::isfinite(v); }

}  // namespace

std::vector<Speaker> layout_speakers(uint32_t channels, uint32_t speaker_mask) {
    const uint32_t count = std::min(channels, kMaskChannels);
    const uint32_t mask = effective_mask(channels, speaker_mask);
    std::vector<Speaker> out;
    for (uint32_t i = 0; i < 31 && out.size() < count; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        if ((mask & bit) == 0) continue;
        Speaker s;
        s.channel = static_cast<uint32_t>(out.size());
        s.bit = bit;
        for (const Position& p : kPositions) {
            if (p.bit == bit) {
                s.code = p.code;
                s.name = p.name;
            }
        }
        if (s.code.empty()) {
            s.bit = 0;
            s.code = std::to_string(s.channel + 1);
            s.name = s.code;
        }
        out.push_back(s);
    }
    while (out.size() < count) {
        Speaker s;
        s.channel = static_cast<uint32_t>(out.size());
        s.code = std::to_string(s.channel + 1);
        s.name = s.code;
        out.push_back(s);
    }
    return out;
}

ChannelMask layout_channel_mask(uint32_t channels) {
    return channels >= kMaskChannels ? ~ChannelMask{0} : (ChannelMask{1} << channels) - 1;
}

ChannelMask codes_mask(const std::vector<std::string>& codes, uint32_t channels, uint32_t speaker_mask) {
    ChannelMask mask = 0;
    for (const Speaker& s : layout_speakers(channels, speaker_mask)) {
        if (std::find(codes.begin(), codes.end(), s.code) != codes.end()) mask |= ChannelMask{1} << s.channel;
    }
    return mask;
}

std::vector<ResolvedGroup> layout_groups(uint32_t channels, uint32_t speaker_mask, const std::vector<SpeakerGroup>& user) {
    std::vector<ResolvedGroup> out;
    const auto add = [&](const std::string& name, ChannelMask mask, bool builtin) {
        if (mask != 0) out.push_back({name, mask, builtin});
    };
    add("All", layout_channel_mask(channels), true);
    add("Front", codes_mask({"L", "C", "R"}, channels, speaker_mask), true);
    add("Surround", codes_mask({"SL", "SR", "RL", "RR"}, channels, speaker_mask), true);
    add("Sub", codes_mask({"LFE"}, channels, speaker_mask), true);
    for (const SpeakerGroup& g : user) add(g.name, codes_mask(g.codes, channels, speaker_mask), false);
    return out;
}

std::string target_label(ChannelMask band_channels, uint32_t channels, uint32_t speaker_mask,
                         const std::vector<SpeakerGroup>& user) {
    const ChannelMask all = layout_channel_mask(channels);
    const ChannelMask mask = band_channels == kAllChannels ? all : band_channels & all;
    if (mask == 0) return "None";
    for (const ResolvedGroup& g : layout_groups(channels, speaker_mask, user)) {
        if (g.mask == mask) return g.name;
    }
    const std::vector<Speaker> speakers = layout_speakers(channels, speaker_mask);
    if (std::popcount(mask) == 1) return speakers[static_cast<size_t>(std::countr_zero(mask))].name;
    std::string label;
    for (const Speaker& s : speakers) {
        if ((mask & (ChannelMask{1} << s.channel)) != 0) label += (label.empty() ? "" : " ") + s.code;
    }
    return label;
}

ChannelMask solo_mute_mask(const SpeakerSetup& setup, uint32_t channels, uint32_t speaker_mask, int solo_channel) {
    const uint32_t count = std::min(channels, kMaskChannels);
    if (solo_channel < 0 || static_cast<uint32_t>(solo_channel) >= count) return 0;
    const ChannelMask solo = ChannelMask{1} << static_cast<uint32_t>(solo_channel);
    ChannelMask muted = layout_channel_mask(channels) & ~solo;
    const int lfe = speaker_channel(effective_mask(channels, speaker_mask), channels, kSpeakerLowFrequency);
    if (lfe >= 0 && setup.bass_management && (setup.small_speakers & solo) != 0) {
        muted &= ~(ChannelMask{1} << static_cast<uint32_t>(lfe));
    }
    return muted;
}

void apply_live_overrides(const LiveOverrides& overrides, EqState* state) {
    state->speakers.muted |= overrides.solo_muted;
    if (!overrides.test_tones) return;
    state->bypass = true;
    state->speakers.upmix = Upmix::Off;
    state->speakers.swap_front_rear = false;
    state->speakers.swap_left_right = false;
}

double delay_ms_for_distance(double farthest_m, double distance_m) {
    return std::max(0.0, farthest_m - distance_m) / kSpeedOfSoundMps * 1000.0;
}

double distance_for_delay(double farthest_m, double delay_ms) {
    return std::max(0.0, farthest_m - delay_ms / 1000.0 * kSpeedOfSoundMps);
}

void set_speaker_distance(SpeakerSetup* setup, uint32_t channels, double* farthest_m, uint32_t channel,
                          double distance_m) {
    const uint32_t count = std::min(channels, kMaxChannels);
    if (channel >= count || !finite(distance_m)) return;
    double distance[kMaxChannels] = {};
    for (uint32_t c = 0; c < count; ++c) distance[c] = distance_for_delay(*farthest_m, setup->delay_ms[c]);
    distance[channel] = std::clamp(distance_m, 0.0, kMaxDistanceM);
    double farthest = 0.0;
    for (uint32_t c = 0; c < count; ++c) farthest = std::max(farthest, distance[c]);
    for (uint32_t c = 0; c < count; ++c) setup->delay_ms[c] = delay_ms_for_distance(farthest, distance[c]);
    *farthest_m = farthest;
}

void set_speaker_delay(SpeakerSetup* setup, double* farthest_m, uint32_t channel, double delay_ms) {
    if (channel >= kMaxChannels || !finite(delay_ms)) return;
    const double ms = std::clamp(delay_ms, 0.0, kMaxSpeakerDelayMs);
    setup->delay_ms[channel] = ms;
    *farthest_m = std::max(*farthest_m, ms / 1000.0 * kSpeedOfSoundMps);
}

namespace {

double snap(double hz, double lo, double hi) {
    if (!finite(hz)) return hz;
    return std::clamp(std::round(hz / kBassStepHz) * kBassStepHz, lo, hi);
}

}  // namespace

double snap_crossover_hz(double hz) { return snap(hz, kCrossoverMinHz, kCrossoverMaxHz); }
double snap_lfe_lowpass_hz(double hz) { return snap(hz, kLfeLowpassMinHz, kLfeLowpassMaxHz); }

EqState saved_with_speakers(const EqState* saved, const EqState& live) {
    EqState out = saved ? *saved : EqState{};
    remap_channels(&out, ChannelLayout{live.layout_channels, live.layout_speaker_mask});
    out.layout_channels = live.layout_channels;
    out.layout_speaker_mask = live.layout_speaker_mask;
    out.speakers = live.speakers;
    std::copy(std::begin(live.channel_gain_db), std::end(live.channel_gain_db), std::begin(out.channel_gain_db));
    return out;
}

unsigned long save_speaker_setup(const std::filesystem::path& path, const EqState& live) {
#if defined(_WIN32)
    if (path.empty()) return ERROR_INVALID_NAME;
    namespace transport = isotone::win;
    const std::wstring file = path.wstring();
#else
    if (path.empty()) return EINVAL;
    namespace transport = isotone::posix;
    const std::string file = path.string();
#endif
    ParamBlock block{};
    EqState saved;
    const bool have = transport::read_persisted_state(file, &block) == transport::PersistedRead::Loaded;
    if (have) from_param_block(block, &saved);
    ParamBlock merged{};
    to_param_block(saved_with_speakers(have ? &saved : nullptr, live), &merged);
    return static_cast<unsigned long>(transport::write_persisted_state(file, merged));
}

bool band_in_view(const Band& band, uint32_t channels, ChannelMask view) {
    const ChannelMask all = layout_channel_mask(channels);
    const ChannelMask v = view == 0 ? all : view & all;
    const ChannelMask b = band.channels == kAllChannels ? all : band.channels & all;
    return (v & b) != 0;
}

uint32_t primary_view_channel(const EqState& state, uint32_t channels, ChannelMask view) {
    const uint32_t count = std::min(channels, kMaskChannels);
    const ChannelMask v = view == 0 ? layout_channel_mask(channels) : view & layout_channel_mask(channels);
    uint32_t best = 0;
    int best_bands = -1;
    for (uint32_t c = 0; c < count; ++c) {
        if ((v & (ChannelMask{1} << c)) == 0) continue;
        const int n = static_cast<int>(std::count_if(state.bands.begin(), state.bands.end(),
                                                     [&](const Band& b) { return band_affects_channel(b, c); }));
        if (n > best_bands) {
            best = c;
            best_bands = n;
        }
    }
    return best;
}

uint32_t band_view_channel(const Band& band, uint32_t channels, ChannelMask view, uint32_t primary) {
    if (band_affects_channel(band, primary)) return primary;
    const uint32_t count = std::min(channels, kMaskChannels);
    const ChannelMask v = view == 0 ? layout_channel_mask(channels) : view & layout_channel_mask(channels);
    for (uint32_t c = 0; c < count; ++c) {
        if ((v & (ChannelMask{1} << c)) != 0 && band_affects_channel(band, c)) return c;
    }
    return primary;
}

}  // namespace isotone::ui
