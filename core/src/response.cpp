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

// Product of H(e^{jw}) over the bands that act on this channel. Cascading
// filters multiplies their transfer functions, which is why magnitudes add in dB
// and phases add in degrees.
std::complex<double> composite_at(const EqState& state, uint32_t channel, double freq,
                                  double sample_rate) {
    std::complex<double> h{1.0, 0.0};
    for (const Band& band : state.bands) {
        if (!band.enabled || !band_affects_channel(band, channel)) {
            continue;
        }
        h *= response(design(band, sample_rate), freq, sample_rate);
    }
    return h;
}

// Every gain that is flat in frequency: preamp, the channel trim, mute.
// Bypass removes the preamp with the bands; trim and mute stay.
double flat_gain_db(const EqState& state, uint32_t channel) {
    if (state.mute) {
        return -std::numeric_limits<double>::infinity();
    }
    double db = state.bypass ? 0.0 : state.preamp_db;
    if (channel < kMaxChannels) {
        db += state.channel_gain_db[channel];
    }
    return db;
}

}  // namespace

void magnitude_db(const EqState& state, uint32_t channel, const double* freqs, size_t n,
                  double sample_rate, double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    const double flat = flat_gain_db(state, channel);
    for (size_t i = 0; i < n; ++i) {
        const double mag = state.bypass ? 1.0 : std::abs(composite_at(state, channel, freqs[i], sample_rate));
        out[i] = flat + 20.0 * std::log10(mag);
    }
}

void phase_deg(const EqState& state, uint32_t channel, const double* freqs, size_t n,
               double sample_rate, double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    if (state.bypass) {
        std::fill(out, out + n, 0.0);
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = std::arg(composite_at(state, channel, freqs[i], sample_rate)) * kRadToDeg;
    }
}

void band_magnitude_db(const Band& band, const double* freqs, size_t n, double sample_rate,
                       double* out) {
    if (freqs == nullptr || out == nullptr) {
        return;
    }
    const BiquadCoeffs c = design(band, sample_rate);
    for (size_t i = 0; i < n; ++i) {
        out[i] = magnitude_db(c, freqs[i], sample_rate);
    }
}

double composite_peak_db(const EqState& state, uint32_t channels, uint32_t speaker_mask,
                         const double* freqs, size_t n, double sample_rate) {
    if (channels == 0) {
        return 0.0;
    }
    double peak = -std::numeric_limits<double>::infinity();

    // The speaker stages as the processor runs them, before the bands: routing,
    // then bass management (a small speaker keeps what is above the crossover
    // and the LFE channel adds what is below, to its own low-passed content).
    const SpeakerSetup& sp = state.speakers;
    const uint32_t mask = speaker_mask != 0 ? speaker_mask : default_speaker_mask(channels);
    const uint32_t routed = std::min(channels, kMaxChannels);
    double routing[kMaxChannels][kMaxChannels];
    routing_matrix(sp, mask, channels, routing);
    const int lfe = speaker_channel(mask, channels, kSpeakerLowFrequency);
    const bool managed = sp.bass_management && lfe >= 0;
    const auto bass_hz = [](double hz, double fallback) {
        return std::clamp(std::isfinite(hz) ? hz : fallback, kMinBassHz, kMaxBassHz);
    };
    const BiquadCoeffs xover_lp = butterworth2(FilterType::LowPass, bass_hz(sp.crossover_hz, 80.0), sample_rate);
    const BiquadCoeffs xover_hp = butterworth2(FilterType::HighPass, bass_hz(sp.crossover_hz, 80.0), sample_rate);
    const BiquadCoeffs lfe_lp = butterworth2(FilterType::LowPass, bass_hz(sp.lfe_lowpass_hz, 120.0), sample_rate);
    const auto small = [&](uint32_t c) {
        return managed && static_cast<int>(c) != lfe && (sp.small_speakers & (ChannelMask{1} << c)) != 0;
    };

    // Sum over inputs of the magnitude of every path from that input into `ch`.
    auto speaker_gain = [&](uint32_t ch, double freq) {
        if (ch >= routed) {
            return 1.0;
        }
        const std::complex<double> lp = std::pow(response(xover_lp, freq, sample_rate), 2);
        const std::complex<double> hp = std::pow(response(xover_hp, freq, sample_rate), 2);
        const std::complex<double> lfe_own = std::pow(response(lfe_lp, freq, sample_rate), 2);
        double sum = 0.0;
        for (uint32_t i = 0; i < routed; ++i) {
            std::complex<double> path = routing[ch][i];
            if (small(ch)) {
                path *= hp;
            } else if (managed && static_cast<int>(ch) == lfe) {
                double redirected = 0.0;
                for (uint32_t c = 0; c < routed; ++c) {
                    if (small(c)) redirected += routing[c][i];
                }
                path = path * lfe_own + redirected * lp;
            }
            sum += std::abs(path);
        }
        return sum;
    };

    auto consider = [&](uint32_t ch, double freq) {
        if (ch < kMaskChannels && (sp.muted & (ChannelMask{1} << ch)) != 0) {
            return;
        }
        // Bypass removes the bands; the speaker stages and trims still apply.
        const double bands = state.bypass ? 1.0 : std::abs(composite_at(state, ch, freq, sample_rate));
        const double mag = bands * speaker_gain(ch, freq);
        double db = 20.0 * std::log10(mag);
        if (ch < kMaxChannels) {
            db += state.channel_gain_db[ch];
        }
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
        for (const Band& band : state.bands) {
            if (band.enabled && band_affects_channel(band, ch)) {
                consider(ch, clamp_fc(band.fc, sample_rate));
            }
        }
    }
    return std::isfinite(peak) ? peak : 0.0;
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
