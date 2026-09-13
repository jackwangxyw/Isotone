// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Adversarial tests for the click-free requirement (plan 4.4). The ramp test in
// test_processor.cpp moves gain in small increments, which is the easy case.
// These move it in one step, which is what actually happens when a user loads a
// preset, toggles a band, or hits A/B in the EQ-by-ear tool.
//
// Every test here first proves the detector can see a click, so none of them can
// pass by accident.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "isotone/processor.h"

using namespace isotone;

namespace {

constexpr double kFs   = 48000.0;
constexpr double kPi   = 3.14159265358979323846;
constexpr double kFreq = 1000.0;
constexpr uint32_t kBlock = 64;

// The largest step a clean sine of this amplitude can take between samples.
double sine_step_limit(double amplitude) {
    return amplitude * 2.0 * std::sin(kPi * kFreq / kFs);
}

double worst_step(const std::vector<float>& x) {
    double worst = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) -
                                         static_cast<double>(x[i - 1])));
    }
    return worst;
}

Band peaking(double fc, double gain_db, double q) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    return b;
}

// Plays a sine for `seconds`, calling `mutate(pos)` before each block so a test
// can change the state mid-stream. Returns the left channel output.
template <typename F>
std::vector<float> render(EqState& s, double seconds, F&& mutate) {
    Processor p;
    p.initialize(kFs, 2, kBlock);
    p.set_target(s);
    p.reset();

    const uint32_t total = static_cast<uint32_t>(kFs * seconds);
    std::vector<float> out;
    out.reserve(total);
    std::vector<float> l(kBlock), r(kBlock);
    float* ptr[2] = {l.data(), r.data()};

    for (uint32_t pos = 0; pos < total; pos += kBlock) {
        if (mutate(pos)) {
            p.set_target(s);
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            const double t = static_cast<double>(pos + i);
            l[i] = r[i] = static_cast<float>(std::sin(2.0 * kPi * kFreq * t / kFs));
        }
        p.process(ptr, kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

}  // namespace

TEST_CASE("the click detector actually detects a click") {
    // Synthesise the failure this suite exists to prevent: a sine whose gain is
    // multiplied instantly partway through. If worst_step cannot see this, none
    // of the tests below mean anything.
    std::vector<float> x(4096);
    for (size_t i = 0; i < x.size(); ++i) {
        double v = std::sin(2.0 * kPi * kFreq * static_cast<double>(i) / kFs);
        if (i >= 2048) {
            v *= 0.25;
        }
        x[i] = static_cast<float>(v);
    }
    // The worst case depends on where in the cycle the step lands; sweep the
    // offset so at least one lands near a peak.
    double worst_over_offsets = 0.0;
    for (size_t off = 0; off < 48; ++off) {
        std::vector<float> y(4096);
        for (size_t i = 0; i < y.size(); ++i) {
            double v = std::sin(2.0 * kPi * kFreq * static_cast<double>(i + off) / kFs);
            if (i >= 2048) {
                v *= 0.25;
            }
            y[i] = static_cast<float>(v);
        }
        worst_over_offsets = std::max(worst_over_offsets, worst_step(y));
    }
    CAPTURE(sine_step_limit(1.0));
    CAPTURE(worst_over_offsets);
    CHECK(worst_over_offsets > sine_step_limit(1.0) * 3.0);
}

TEST_CASE("an instant 12 dB gain jump does not click") {
    EqState s;
    s.bands.push_back(peaking(kFreq, 0.0, 1.0));

    bool changed = false;
    const std::vector<float> out = render(s, 0.4, [&](uint32_t pos) {
        if (pos >= static_cast<uint32_t>(kFs * 0.1) && !changed) {
            s.bands[0].gain_db = -12.0;
            changed = true;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CAPTURE(sine_step_limit(1.0));
    CHECK(worst_step(out) < sine_step_limit(1.0) * 1.10);

    const double tail = *std::max_element(out.end() - 4096, out.end());
    CHECK(std::abs(20.0 * std::log10(tail) + 12.0) < 0.05);
}

TEST_CASE("an instant frequency jump across three octaves does not click") {
    EqState s;
    s.bands.push_back(peaking(300.0, 12.0, 2.0));

    bool changed = false;
    const std::vector<float> out = render(s, 0.5, [&](uint32_t pos) {
        if (pos >= static_cast<uint32_t>(kFs * 0.15) && !changed) {
            s.bands[0].fc = 2400.0;
            changed = true;
            return true;
        }
        return false;
    });

    // The band starts away from 1 kHz, sweeps across it, and ends away again, so
    // the sine's amplitude rises and falls. Bound by the peak gain it can reach.
    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(std::pow(10.0, 12.0 / 20.0)) * 1.10);
}

TEST_CASE("an instant Q jump does not click") {
    EqState s;
    s.bands.push_back(peaking(kFreq, 9.0, 0.5));

    bool changed = false;
    const std::vector<float> out = render(s, 0.4, [&](uint32_t pos) {
        if (pos >= static_cast<uint32_t>(kFs * 0.1) && !changed) {
            s.bands[0].width = 20.0;
            changed = true;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(std::pow(10.0, 9.0 / 20.0)) * 1.10);
}

TEST_CASE("toggling bypass does not click") {
    EqState s;
    s.bands.push_back(peaking(kFreq, -18.0, 1.0));

    int stage = 0;
    const std::vector<float> out = render(s, 0.6, [&](uint32_t pos) {
        if (stage == 0 && pos >= static_cast<uint32_t>(kFs * 0.15)) {
            s.bypass = true;
            stage = 1;
            return true;
        }
        if (stage == 1 && pos >= static_cast<uint32_t>(kFs * 0.35)) {
            s.bypass = false;
            stage = 2;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(1.0) * 1.10);
    CHECK(stage == 2);
}

TEST_CASE("toggling mute does not click") {
    EqState s;
    int stage = 0;
    const std::vector<float> out = render(s, 0.6, [&](uint32_t pos) {
        if (stage == 0 && pos >= static_cast<uint32_t>(kFs * 0.15)) {
            s.mute = true;
            stage = 1;
            return true;
        }
        if (stage == 1 && pos >= static_cast<uint32_t>(kFs * 0.35)) {
            s.mute = false;
            stage = 2;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(1.0) * 1.10);
    CHECK(stage == 2);
}

TEST_CASE("disabling and re-enabling a band does not click") {
    EqState s;
    s.bands.push_back(peaking(kFreq, 12.0, 1.0));

    int stage = 0;
    const std::vector<float> out = render(s, 0.6, [&](uint32_t pos) {
        if (stage == 0 && pos >= static_cast<uint32_t>(kFs * 0.15)) {
            s.bands[0].enabled = false;
            stage = 1;
            return true;
        }
        if (stage == 1 && pos >= static_cast<uint32_t>(kFs * 0.35)) {
            s.bands[0].enabled = true;
            stage = 2;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(std::pow(10.0, 12.0 / 20.0)) * 1.10);
    CHECK(stage == 2);
}

TEST_CASE("loading a whole different preset does not click") {
    // The realistic worst case: every band changes at once, as when the user
    // picks a different preset from the list.
    EqState s;
    s.bands.push_back(peaking(80.0, 6.0, 0.7));
    s.bands.push_back(peaking(kFreq, -9.0, 2.0));
    s.bands.push_back(peaking(7000.0, 4.0, 1.5));
    s.preamp_db = -3.0;

    bool changed = false;
    const std::vector<float> out = render(s, 0.5, [&](uint32_t pos) {
        if (pos >= static_cast<uint32_t>(kFs * 0.15) && !changed) {
            s.bands.clear();
            Band hs;
            hs.type = FilterType::HighShelf;
            hs.fc = 4000.0;
            hs.gain_db = -6.0;
            hs.width = 0.7;
            s.bands.push_back(hs);
            s.bands.push_back(peaking(kFreq, 10.0, 4.0));
            s.preamp_db = -8.0;
            changed = true;
            return true;
        }
        return false;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(std::pow(10.0, 10.0 / 20.0)) * 1.10);
}

TEST_CASE("a fast drag of 200 updates per second stays smooth") {
    // What the UI actually does while a handle is being dragged: many small
    // updates in quick succession. Each one must not restart a crossfade.
    EqState s;
    s.bands.push_back(peaking(kFreq, 0.0, 1.0));

    const std::vector<float> out = render(s, 1.0, [&](uint32_t pos) {
        const double t = static_cast<double>(pos) / kFs;
        s.bands[0].gain_db = -12.0 * 0.5 * (1.0 - std::cos(2.0 * kPi * 1.5 * t));
        return true;
    });

    CAPTURE(worst_step(out));
    CHECK(worst_step(out) < sine_step_limit(1.0) * 1.10);
}
