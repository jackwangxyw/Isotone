// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/response.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#include "isotone/apo_config.h"
#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"

namespace isotone {
namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

// A band the curve draws: the processor's effective values, designed once.
struct Designed {
    Band         band;
    BiquadCoeffs coeffs;
};

// The enabled bands of `state`, each designed once per call rather than once
// per frequency.
std::vector<Designed> design_bands(const EqState& state, double sample_rate) {
    std::vector<Designed> out;
    for (const Band& given : state.bands) {
        const Band band = effective_band(given);
        if (band.enabled) {
            out.push_back({band, design(band, sample_rate)});
        }
    }
    return out;
}

// Product of H(e^{jw}) over the bands that act on this channel. Cascading
// filters multiplies their transfer functions, which is why magnitudes add in dB
// and phases add in degrees.
std::complex<double> composite_at(const std::vector<Designed>& bands, uint32_t channel, double freq,
                                  double sample_rate) {
    std::complex<double> h{1.0, 0.0};
    for (const Designed& d : bands) {
        if (!band_affects_channel(d.band, channel)) {
            continue;
        }
        h *= response(d.coeffs, freq, sample_rate);
    }
    return h;
}

// A channel's trim as the processor plays it; channels past the table have none.
double trim_db(const EqState& state, uint32_t channel) {
    return channel < kMaxChannels ? effective_level_db(state.channel_gain_db[channel]) : 0.0;
}

// Every gain that is flat in frequency: preamp, the channel trim, mute.
// Bypass removes the preamp with the bands; trim and mute stay.
double flat_gain_db(const EqState& state, uint32_t channel) {
    if (state.mute) {
        return -std::numeric_limits<double>::infinity();
    }
    return (state.bypass ? 0.0 : effective_level_db(state.preamp_db)) + trim_db(state, channel);
}

// The speaker stages for a stream layout, as the processor sets them up.
struct SpeakerStages {
    const SpeakerSetup* sp = nullptr;
    uint32_t mask = 0;
    uint32_t routed = 0;   // channels the routing and bass stages cover
    int lfe = -1;
    bool managed = false;
    BiquadCoeffs xover_lp, xover_hp, lfe_lp;

    bool small(uint32_t c) const {
        return managed && c < routed && static_cast<int>(c) != lfe && (sp->small_speakers & (ChannelMask{1} << c)) != 0;
    }
    bool muted(uint32_t c) const { return c < kMaskChannels && (sp->muted & (ChannelMask{1} << c)) != 0; }
    bool inverted(uint32_t c) const { return c < kMaskChannels && (sp->inverted & (ChannelMask{1} << c)) != 0; }
};

SpeakerStages speaker_stages(const EqState& state, uint32_t channels, uint32_t speaker_mask, double sample_rate) {
    SpeakerStages st;
    st.sp = &state.speakers;
    st.mask = speaker_mask != 0 ? speaker_mask : default_speaker_mask(channels);
    st.routed = std::min(channels, kMaxChannels);
    st.lfe = speaker_channel(st.mask, channels, kSpeakerLowFrequency);
    st.managed = state.speakers.bass_management && st.lfe >= 0;
    const auto bass_hz = [](double hz, double fallback) {
        return std::clamp(std::isfinite(hz) ? hz : fallback, kMinBassHz, kMaxBassHz);
    };
    st.xover_lp = butterworth2(FilterType::LowPass, bass_hz(state.speakers.crossover_hz, 80.0), sample_rate);
    st.xover_hp = butterworth2(FilterType::HighPass, bass_hz(state.speakers.crossover_hz, 80.0), sample_rate);
    st.lfe_lp = butterworth2(FilterType::LowPass, bass_hz(state.speakers.lfe_lowpass_hz, 120.0), sample_rate);
    return st;
}

// An output's own path through the speaker stages: a small speaker's high-pass,
// the LFE channel's low-pass, and polarity. Each filter is two identical
// sections, a 24 dB/oct Linkwitz-Riley.
std::complex<double> own_path_at(const SpeakerStages& st, uint32_t channel, double freq, double sample_rate) {
    std::complex<double> h{st.inverted(channel) ? -1.0 : 1.0, 0.0};
    if (st.small(channel)) {
        h *= std::pow(response(st.xover_hp, freq, sample_rate), 2);
    } else if (st.managed && static_cast<int>(channel) == st.lfe) {
        h *= std::pow(response(st.lfe_lp, freq, sample_rate), 2);
    }
    return h;
}

// `state` as the engine plays it on this layout: a state written for another
// layout is moved by speaker role first, as IsoAPO and the Equalizer APO
// backend move it. Copies only when the layouts differ.
class OnLayout {
public:
    OnLayout(const EqState& state, uint32_t channels, uint32_t speaker_mask) : state_(&state) {
        const auto mask = [](uint32_t c, uint32_t m) { return m != 0 ? m : default_speaker_mask(c); };
        if (state.layout_channels != 0 && channels != 0 &&
            (state.layout_channels != channels ||
             mask(state.layout_channels, state.layout_speaker_mask) != mask(channels, speaker_mask))) {
            moved_ = state;
            remap_channels(&moved_, ChannelLayout{channels, speaker_mask});
            state_ = &moved_;
        }
    }
    const EqState& operator*() const { return *state_; }

private:
    const EqState* state_;
    EqState moved_;
};

}  // namespace

void magnitude_db(const EqState& given, uint32_t channels, uint32_t speaker_mask, uint32_t channel,
                  const double* freqs, size_t n, double sample_rate, double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    const OnLayout on(given, channels, speaker_mask);
    const EqState& state = *on;
    const SpeakerStages st = speaker_stages(state, channels, speaker_mask, sample_rate);
    const double flat = st.muted(channel) ? -std::numeric_limits<double>::infinity() : flat_gain_db(state, channel);
    const std::vector<Designed> bands = design_bands(state, sample_rate);
    for (size_t i = 0; i < n; ++i) {
        // Bypass removes the bands; the speaker stages stay.
        std::complex<double> h = own_path_at(st, channel, freqs[i], sample_rate);
        if (!state.bypass) h *= composite_at(bands, channel, freqs[i], sample_rate);
        out[i] = flat + 20.0 * std::log10(std::abs(h));
    }
}

void phase_deg(const EqState& given, uint32_t channels, uint32_t speaker_mask, uint32_t channel,
               const double* freqs, size_t n, double sample_rate, double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    const OnLayout on(given, channels, speaker_mask);
    const EqState& state = *on;
    const SpeakerStages st = speaker_stages(state, channels, speaker_mask, sample_rate);
    const std::vector<Designed> bands = design_bands(state, sample_rate);
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> h = own_path_at(st, channel, freqs[i], sample_rate);
        if (!state.bypass) h *= composite_at(bands, channel, freqs[i], sample_rate);
        out[i] = std::arg(h) * kRadToDeg;
    }
}

void band_magnitude_db(const Band& band, const double* freqs, size_t n, double sample_rate,
                       double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    const BiquadCoeffs c = design(effective_band(band), sample_rate);
    for (size_t i = 0; i < n; ++i) {
        out[i] = magnitude_db(c, freqs[i], sample_rate);
    }
}

double composite_peak_db(const EqState& given, uint32_t channels, uint32_t speaker_mask,
                         const double* freqs, size_t n, double sample_rate) {
    if (channels == 0) {
        return 0.0;
    }
    const OnLayout on(given, channels, speaker_mask);
    const EqState& state = *on;
    double peak = -std::numeric_limits<double>::infinity();

    // The speaker stages as the processor runs them, before the bands: routing,
    // then bass management (a small speaker keeps what is above the crossover
    // and the LFE channel adds what is below, to its own low-passed content).
    const SpeakerStages st = speaker_stages(state, channels, speaker_mask, sample_rate);
    const uint32_t routed = st.routed;
    double routing[kMaxChannels][kMaxChannels];
    routing_matrix(state.speakers, st.mask, channels, routing);

    // Sum over inputs of the magnitude of every path from that input into `ch`.
    auto speaker_gain = [&](uint32_t ch, double freq) {
        if (ch >= routed) {
            return 1.0;
        }
        const std::complex<double> lp = std::pow(response(st.xover_lp, freq, sample_rate), 2);
        const std::complex<double> hp = std::pow(response(st.xover_hp, freq, sample_rate), 2);
        const std::complex<double> lfe_own = std::pow(response(st.lfe_lp, freq, sample_rate), 2);
        double sum = 0.0;
        for (uint32_t i = 0; i < routed; ++i) {
            std::complex<double> path = routing[ch][i];
            if (st.small(ch)) {
                path *= hp;
            } else if (st.managed && static_cast<int>(ch) == st.lfe) {
                // A muted small speaker sends the sub nothing.
                double redirected = 0.0;
                for (uint32_t c = 0; c < routed; ++c) {
                    if (st.small(c) && !st.muted(c)) redirected += routing[c][i];
                }
                path = path * lfe_own + redirected * lp;
            }
            sum += std::abs(path);
        }
        return sum;
    };

    const std::vector<Designed> designed = design_bands(state, sample_rate);
    auto consider = [&](uint32_t ch, double freq) {
        if (st.muted(ch)) {
            return;
        }
        // Bypass removes the bands; the speaker stages and trims still apply.
        const double bands = state.bypass ? 1.0 : std::abs(composite_at(designed, ch, freq, sample_rate));
        const double mag = bands * speaker_gain(ch, freq);
        const double db = 20.0 * std::log10(mag) + trim_db(state, ch);
        if (db > peak) {
            peak = db;
        }
    };

    for (uint32_t ch = 0; ch < channels; ++ch) {
        for (size_t i = 0; i < n; ++i) {
            consider(ch, freqs[i]);
        }
        // A high-Q bell can sit entirely between two grid points, so sample each
        // band centre too. Use the design frequency after clamping.
        for (const Designed& d : designed) {
            if (band_affects_channel(d.band, ch) && std::isfinite(d.band.fc)) {
                consider(ch, clamp_fc(d.band.fc, sample_rate));
            }
        }
    }
    return std::isfinite(peak) ? peak : 0.0;
}

double auto_preamp_db(const EqState& state, uint32_t channels, uint32_t speaker_mask,
                      const double* freqs, size_t n, double sample_rate) {
    return std::min(0.0, -composite_peak_db(state, channels, speaker_mask, freqs, n, sample_rate));
}

std::vector<double> log_grid(double f_lo, double f_hi, size_t count) {
    std::vector<double> grid;
    if (count == 0 || !(f_lo > 0.0) || !(f_hi > f_lo)) {
        return grid;
    }
    grid.resize(count);
    if (count == 1) {
        grid[0] = f_lo;
        return grid;
    }
    const double step = std::log(f_hi / f_lo) / static_cast<double>(count - 1);
    for (size_t i = 0; i < count; ++i) {
        grid[i] = f_lo * std::exp(step * static_cast<double>(i));
    }
    return grid;
}

}  // namespace isotone
