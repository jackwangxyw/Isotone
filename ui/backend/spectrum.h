// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The spectrum behind the graph (plan 7.4): the processed output, mixed to mono,
// Hann-windowed FFT of the newest 8192 samples, levels in dBFS where a full-scale
// sine reads 0, smoothed like a meter (fast attack, 300 ms release).
//
// Settings, General, Spectrum: the FFT size (4096, 8192 or 16384), the release,
// a peak-hold line (the loudest level, falling kPeakFallDbPerSecond), and a
// tilt added to the shown levels only, 0 dB at 1 kHz.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace isotone::ui {

class SpectrumAnalyzer {
public:
    static constexpr size_t kFftSize = 8192;   // the default resolution
    static constexpr double kFloorDb = -120.0;
    static constexpr double kPeakFallDbPerSecond = 6.0;

    explicit SpectrumAnalyzer(double release_ms = 300.0, double attack_ms = 20.0);

    // A power of two; starts again from silence.
    void set_fft_size(size_t n);
    size_t fft_size() const { return fft_size_; }
    void set_release_ms(double ms) { release_ms_ = ms; }
    // dB per octave added to what levels_at and peak_levels_at give, pivoting at 1 kHz.
    void set_tilt(double db_per_octave) { tilt_ = db_per_octave; }

    // Frames of `channels` interleaved samples, mixed to mono (the mean).
    void push(const float* interleaved, size_t frames, uint32_t channels);

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
    // The same for the peak-hold line.
    void peak_levels_at(const double* freqs, size_t n, double* out_db, Bands bands = Bands::Loudest) const;

    // Silence and the floor, as after construction.
    void reset();

    const std::vector<double>& bin_db() const { return smoothed_; }
    const std::vector<double>& peak_db() const { return peak_; }
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
    std::vector<double> peak_;       // the same, peak hold, dB
    double release_ms_, attack_ms_;
    double tilt_ = 0.0;
    double sample_rate_ = 48000.0;
};

// In-place radix-2 FFT; `x.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& x);

}  // namespace isotone::ui
