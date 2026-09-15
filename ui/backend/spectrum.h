// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The spectrum behind the graph (plan 7.4): the processed output, mixed to mono,
// Hann-windowed FFT of the newest 8192 samples, levels in dBFS where a full-scale
// sine reads 0, smoothed like a meter (fast attack, 300 ms release).

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace isotone::ui {

class SpectrumAnalyzer {
public:
    static constexpr size_t kFftSize = 8192;
    static constexpr double kFloorDb = -120.0;

    explicit SpectrumAnalyzer(double release_ms = 300.0, double attack_ms = 20.0);

    // Frames of `channels` interleaved samples, mixed to mono (the mean).
    void push(const float* interleaved, size_t frames, uint32_t channels);

    // Runs the FFT on the newest kFftSize samples and moves the smoothed levels
    // `elapsed_s` towards them. Before kFftSize samples have arrived the levels
    // stay at the floor.
    void update(double sample_rate, double elapsed_s);

    // Levels at log-spaced display frequencies: the loudest bin within each
    // point's span where bins are dense, interpolated between bins where they
    // are sparse (the bass end).
    void levels_at(const double* freqs, size_t n, double* out_db) const;

    // Silence and the floor, as after construction.
    void reset();

    const std::vector<double>& bin_db() const { return smoothed_; }
    double sample_rate() const { return sample_rate_; }

private:
    std::vector<float> history_;   // circular, kFftSize samples
    size_t write_ = 0;
    size_t filled_ = 0;
    std::vector<double> window_;
    std::vector<std::complex<double>> work_;
    std::vector<double> smoothed_;   // kFftSize / 2 + 1 bins, dB
    std::vector<double> power_;      // the same, smoothed power
    double release_ms_, attack_ms_;
    double sample_rate_ = 48000.0;
};

// In-place radix-2 FFT; `x.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& x);

}  // namespace isotone::ui
