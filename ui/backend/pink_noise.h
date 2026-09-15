// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Pink noise for the per-speaker test tone (docs/ui-spec.md, "Speakers view"):
// -30 dBFS RMS, where 0 dBFS RMS is an RMS of 1.0, the level home-theatre test
// discs use. A periodic loop built in the frequency domain: every bin from 20 Hz
// to 20 kHz with a power of 1/f (-3 dB per octave) and a random phase, so the
// spectrum is exact, the loop repeats without a seam, and its RMS is scaled to
// the level exactly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace isotone::ui {

inline constexpr double kTestToneRmsDbfs = -30.0;

class PinkNoise {
public:
    // A loop of at least `min_seconds` (a power of two in samples).
    explicit PinkNoise(double sample_rate, double rms_dbfs = kTestToneRmsDbfs, double min_seconds = 4.0,
                       uint32_t seed = 1);

    float next() {
        const float v = loop_[pos_];
        pos_ = (pos_ + 1) % loop_.size();
        return v;
    }
    const std::vector<float>& loop() const { return loop_; }
    double sample_rate() const { return sample_rate_; }

private:
    std::vector<float> loop_;
    size_t pos_ = 0;
    double sample_rate_;
};

}  // namespace isotone::ui
