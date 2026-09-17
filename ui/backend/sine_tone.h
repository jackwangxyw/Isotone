// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The EQ by ear tone: a sine whose frequency, level and on/off follow values set
// from another thread without a click. A full-scale sine is 0 dBFS. The phase runs
// on through every change; the frequency glides to a new one in octaves, the level
// ramps, and on/off is the level fading to or from silence. The auto sweep moves
// the frequency at a rate in octaves per second and turns at 20 Hz and 20 kHz.
//
// The setters and frequency() are for any thread; render() for one at a time.

#pragma once

#include <atomic>
#include <cstdint>

namespace isotone::ui {

inline constexpr double kEarToneDbfs = -30.0;
inline constexpr double kEarToneLowHz = 20.0;
inline constexpr double kEarToneHighHz = 20000.0;

class SineTone {
public:
    static constexpr double kGlideTimeConstantS = 0.020;
    static constexpr double kLevelTimeConstantS = 0.010;

    void set_frequency(double hz);
    void set_level_db(double dbfs) { level_db_ = dbfs; }
    void set_on(bool on) { on_ = on; }
    bool on() const { return on_; }
    // Octaves per second, up when positive; 0 holds the frequency where it is.
    void set_sweep(double octaves_per_second) { sweep_ = octaves_per_second; }
    // Where the last render left the frequency.
    double frequency() const { return frequency_; }

    void render(float* out, uint32_t frames, double sample_rate);

private:
    std::atomic<double> target_hz_{1000.0};
    std::atomic<uint64_t> target_serial_{0};
    std::atomic<double> level_db_{kEarToneDbfs};
    std::atomic<bool> on_{false};
    std::atomic<double> sweep_{0.0};
    std::atomic<double> frequency_{1000.0};

    // The render's own state.
    bool started_ = false;
    uint64_t serial_ = 0;
    double target_octave_ = 0.0;   // log2 Hz
    double octave_ = 0.0;
    double sweep_seen_ = 0.0;
    double direction_ = 1.0;       // the sweep's, flipped at either end
    double amplitude_ = 0.0;
    double phase_ = 0.0;           // radians, in [0, 2 pi)
};

}  // namespace isotone::ui
