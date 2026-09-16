// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "previewsession.h"

#include <cmath>

namespace {

isotone::Band band(uint32_t id, isotone::FilterType type, double fc, double gain, double q) {
    isotone::Band b;
    b.id = id;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain;
    b.width = q;
    return b;
}

// A music-like level: loudest around 150 Hz, falling to either side, with a
// gentle ripple.
double sample_level(double hz) {
    const double octaves = std::abs(std::log2(hz / 150.0));
    return -14.0 - 3.2 * std::pow(octaves, 1.3) + 2.5 * std::sin(std::log2(hz) * 3.1);
}

}  // namespace

PreviewSession::PreviewSession(QObject* parent) : EqSession(parent) {
    using isotone::FilterType;
    isotone::EqState s;
    s.bands = {band(1, FilterType::LowShelf, 105, 6.4, 0.7), band(2, FilterType::Peaking, 8800, 5.1, 1.42),
               band(3, FilterType::Peaking, 118, -3.1, 0.5),  band(4, FilterType::Peaking, 2100, -2.4, 2),
               band(5, FilterType::Peaking, 5300, 1.9, 3.1),  band(6, FilterType::HighShelf, 10000, -2.1, 0.7),
               band(7, FilterType::Peaking, 42, 2.0, 1.2),    band(8, FilterType::Peaking, 260, -1.5, 1.6)};
    loadState(&s);
    select(3);
}

bool PreviewSession::spectrumLevels(const double* freqs, size_t n, double* out_db) const {
    for (size_t i = 0; i < n; ++i) out_db[i] = sample_level(freqs[i]);
    return true;
}
