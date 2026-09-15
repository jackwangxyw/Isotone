// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "pink_noise.h"

#include <cmath>
#include <complex>
#include <random>

#include "spectrum.h"

namespace isotone::ui {

PinkNoise::PinkNoise(double sample_rate, double rms_dbfs, double min_seconds, uint32_t seed) : sample_rate_(sample_rate) {
    size_t n = 1;
    while (static_cast<double>(n) < min_seconds * sample_rate) n <<= 1;

    // Bin k is k * rate / n Hz. Amplitude 1/sqrt(f) is power 1/f; each positive
    // bin's mirror is its conjugate, so the transform is real.
    constexpr double kTwoPi = 6.283185307179586;
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> phase(0.0, kTwoPi);
    std::vector<std::complex<double>> x(n);
    for (size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * sample_rate / static_cast<double>(n);
        const double p = phase(random);   // drawn for every bin, so the phases do not depend on the band
        if (f < 20.0 || f > 20000.0) continue;
        x[k] = std::polar(1.0 / std::sqrt(f), p);
        x[n - k] = std::conj(x[k]);
    }
    fft(x);

    double sum = 0.0;
    for (const std::complex<double>& v : x) sum += v.real() * v.real();
    const double scale = std::pow(10.0, rms_dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
    loop_.resize(n);
    for (size_t i = 0; i < n; ++i) loop_[i] = static_cast<float>(x[i].real() * scale);
}

}  // namespace isotone::ui
