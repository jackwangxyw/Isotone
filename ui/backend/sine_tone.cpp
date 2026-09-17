// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "sine_tone.h"

#include <algorithm>
#include <cmath>

namespace isotone::ui {

namespace {

constexpr double kTwoPi = 6.283185307179586;
// Under this the fading level is silence: -140 dBFS, far below any output's noise.
constexpr double kSilent = 1e-7;

}  // namespace

void SineTone::set_frequency(double hz) {
    target_hz_ = std::clamp(hz, kEarToneLowHz, kEarToneHighHz);
    ++target_serial_;
}

void SineTone::render(float* out, uint32_t frames, double sample_rate) {
    const double low = std::log2(kEarToneLowHz), high = std::log2(kEarToneHighHz);
    const uint64_t serial = target_serial_;
    if (!started_ || serial != serial_) {
        target_octave_ = std::log2(target_hz_.load());
        serial_ = serial;
        // Nothing sounding has nothing to glide from.
        if (!started_ || amplitude_ == 0.0) octave_ = target_octave_;
        started_ = true;
    }
    const double level = on_ ? std::pow(10.0, level_db_ / 20.0) : 0.0;
    const double sweep = sweep_;
    const double glide = 1.0 - std::exp(-1.0 / (kGlideTimeConstantS * sample_rate));
    const double ramp = 1.0 - std::exp(-1.0 / (kLevelTimeConstantS * sample_rate));
    const double step = std::abs(sweep) / sample_rate;
    if (sweep != sweep_seen_) {
        // A rate set, rather than the sweep turning at an end, gives the direction.
        sweep_seen_ = sweep;
        direction_ = sweep < 0.0 ? -1.0 : 1.0;
    }

    for (uint32_t i = 0; i < frames; ++i) {
        if (step > 0.0) {
            // The sweep carries the target and the glide with it, so a sweep does
            // not lag; at an end it turns back by what it went past.
            double moved = target_octave_ + direction_ * step;
            if (moved > high || moved < low) {
                const double edge = moved > high ? high : low;
                moved = 2.0 * edge - moved;
                direction_ = -direction_;
            }
            octave_ += moved - target_octave_;
            target_octave_ = moved;
        }
        octave_ = std::clamp(octave_ + (target_octave_ - octave_) * glide, low, high);
        amplitude_ += (level - amplitude_) * ramp;
        if (level == 0.0 && amplitude_ < kSilent) amplitude_ = 0.0;

        out[i] = static_cast<float>(amplitude_ * std::sin(phase_));
        phase_ += kTwoPi * std::pow(2.0, octave_) / sample_rate;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
    }
    frequency_ = std::pow(2.0, octave_);
}

}  // namespace isotone::ui
