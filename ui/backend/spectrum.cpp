// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "spectrum.h"

#include <algorithm>
#include <cmath>

namespace isotone::ui {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void fft(std::vector<std::complex<double>>& x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer(double release_ms, double attack_ms) : release_ms_(release_ms), attack_ms_(attack_ms) {
    set_fft_size(kFftSize);
}

void SpectrumAnalyzer::set_fft_size(size_t n) {
    fft_size_ = n;
    history_.assign(n, 0.0f);
    work_.assign(n, {});
    window_.resize(n);
    for (size_t i = 0; i < n; ++i) window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    smoothed_.assign(n / 2 + 1, kFloorDb);
    power_.assign(n / 2 + 1, 0.0);
    write_ = 0;
    filled_ = 0;
}

void SpectrumAnalyzer::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(smoothed_.begin(), smoothed_.end(), kFloorDb);
    std::fill(power_.begin(), power_.end(), 0.0);
    write_ = 0;
    filled_ = 0;
}

void SpectrumAnalyzer::push(const float* interleaved, size_t frames, uint32_t channels) {
    if (channels == 0) return;
    for (size_t f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (uint32_t c = 0; c < channels; ++c) sum += interleaved[f * channels + c];
        history_[write_] = static_cast<float>(sum / channels);
        write_ = (write_ + 1) % fft_size_;
    }
    filled_ = std::min(fft_size_, filled_ + frames);
}

void SpectrumAnalyzer::push_silence(size_t frames) {
    const size_t n = std::min(frames, fft_size_);
    for (size_t f = 0; f < n; ++f) {
        history_[write_] = 0.0f;
        write_ = (write_ + 1) % fft_size_;
    }
    filled_ = std::min(fft_size_, filled_ + n);
}

double SpectrumAnalyzer::loudest_db() const {
    return *std::max_element(smoothed_.begin(), smoothed_.end());
}

void SpectrumAnalyzer::update(double sample_rate, double elapsed_s) {
    sample_rate_ = sample_rate;
    if (filled_ < fft_size_) return;
    for (size_t i = 0; i < fft_size_; ++i) work_[i] = {history_[(write_ + i) % fft_size_] * window_[i], 0.0};
    fft(work_);
    // A full-scale sine gives |X| = N/2 * (window mean 0.5): scale so it reads 0 dB.
    const double scale = 2.0 / (static_cast<double>(fft_size_) * 0.5);
    const double release = 1.0 - std::exp(-elapsed_s * 1000.0 / release_ms_);
    const double attack = 1.0 - std::exp(-elapsed_s * 1000.0 / attack_ms_);
    // Smoothed in power, as a meter's ballistics are, then shown in dB.
    const double floor_power = std::pow(10.0, kFloorDb / 10.0);
    for (size_t b = 0; b < smoothed_.size(); ++b) {
        const double magnitude = std::abs(work_[b]) * scale;
        const double power = magnitude * magnitude;
        double& p = power_[b];
        p += (power - p) * (power > p ? attack : release);
        smoothed_[b] = 10.0 * std::log10(std::max(p, floor_power));
    }
}

void SpectrumAnalyzer::levels_at(const double* freqs, size_t n, double* out_db, Bands bands) const {
    sample(smoothed_, freqs, n, out_db, bands);
}

void SpectrumAnalyzer::sample(const std::vector<double>& bins, const double* freqs, size_t n, double* out_db, Bands bands) const {
    const double bin_hz = sample_rate_ / static_cast<double>(fft_size_);
    const size_t last = bins.size() - 1;
    for (size_t i = 0; i < n; ++i) {
        const double centre = freqs[i] / bin_hz;
        // The span halfway to each neighbouring display point.
        const double lo = (i > 0 ? std::sqrt(freqs[i - 1] * freqs[i]) : freqs[i]) / bin_hz;
        const double hi = (i + 1 < n ? std::sqrt(freqs[i] * freqs[i + 1]) : freqs[i]) / bin_hz;
        const size_t first = static_cast<size_t>(std::ceil(lo));
        const size_t end = static_cast<size_t>(std::floor(hi));
        if (end >= first + 1 && end <= last) {
            if (bands == Bands::Loudest) {
                double peak = kFloorDb;
                for (size_t b = first; b <= end; ++b) peak = std::max(peak, bins[b]);
                out_db[i] = peak;
            } else {
                double power = 0.0;
                for (size_t b = first; b <= end; ++b) power += std::pow(10.0, bins[b] / 10.0);
                out_db[i] = 10.0 * std::log10(std::max(power / static_cast<double>(end - first + 1), std::pow(10.0, kFloorDb / 10.0)));
            }
        } else {
            const size_t b0 = std::min(last - 1, static_cast<size_t>(std::floor(centre)));
            const double t = std::clamp(centre - static_cast<double>(b0), 0.0, 1.0);
            out_db[i] = bins[b0] + (bins[b0 + 1] - bins[b0]) * t;
        }
    }
}

}  // namespace isotone::ui
