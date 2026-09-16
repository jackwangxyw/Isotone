// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The spectrum behind the graph (plan 7.4): the processed output, mixed to mono,
// Hann-windowed FFT of the newest kFftSize samples, levels in dBFS where a
// full-scale sine reads 0, smoothed like a meter (fast attack, and a decay the
// settings give).
//
// The resolution is pinned at the highest it offered, and the peak-hold line and
// the tilt are gone (owner, 2026-09-15). Settings, General, Spectrum is the decay
// and how much the drawn curve is smoothed (ResponseGraph).

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace isotone::ui {

class SpectrumAnalyzer {
public:
    static constexpr size_t kFftSize = 16384;   // 2.9 Hz bins at 48 kHz
    static constexpr double kFloorDb = -120.0;

    explicit SpectrumAnalyzer(double release_ms = 300.0, double attack_ms = 20.0);

    // A power of two; starts again from silence. The app keeps kFftSize; the
    // tests use the smaller sizes.
    void set_fft_size(size_t n);
    size_t fft_size() const { return fft_size_; }
    // How long a level takes to fall, as a meter's release. Settings, General,
    // Spectrum, Decay.
    void set_release_ms(double ms) { release_ms_ = ms; }
    double release_ms() const { return release_ms_; }

    // Frames of `channels` interleaved samples, mixed to mono (the mean).
    void push(const float* interleaved, size_t frames, uint32_t channels);
    // The silence a source that has gone idle is no longer sending: the levels
    // then fall as they would on real silence instead of standing still.
    void push_silence(size_t frames);

    // Runs the FFT on the newest fft_size() samples and moves the smoothed levels
    // `elapsed_s` towards them. Before fft_size() samples have arrived the levels
    // stay at the floor.
    void update(double sample_rate, double elapsed_s);

    // How a display point takes the bins in its span: the loudest one (a tone
    // reads its own level) or their mean power (a smooth curve, what the graph
    // draws). Where bins are sparser than the points, both interpolate.
    enum class Bands { Loudest, Mean };

    // Levels at log-spaced display frequencies.
    void levels_at(const double* freqs, size_t n, double* out_db, Bands bands = Bands::Loudest) const;

    // Silence and the floor, as after construction.
    void reset();

    // The loudest smoothed bin, dB: what the display asks to decide whether
    // anything is left to draw.
    double loudest_db() const;

    const std::vector<double>& bin_db() const { return smoothed_; }
    double sample_rate() const { return sample_rate_; }

private:
    void sample(const std::vector<double>& bins, const double* freqs, size_t n, double* out_db, Bands bands) const;

    size_t fft_size_ = kFftSize;
    std::vector<float> history_;   // circular, fft_size_ samples
    size_t write_ = 0;
    size_t filled_ = 0;
    std::vector<double> window_;
    std::vector<std::complex<double>> work_;
    std::vector<double> smoothed_;   // fft_size_ / 2 + 1 bins, dB
    std::vector<double> power_;      // the same, smoothed power
    double release_ms_, attack_ms_;
    double sample_rate_ = 48000.0;
};

// In-place radix-2 FFT; `x.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& x);

}  // namespace isotone::ui
