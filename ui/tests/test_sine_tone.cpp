// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The EQ by ear tone without an endpoint: its level and frequency, the glide and
// fades that keep a change from clicking, the auto sweep, and a render that does
// not depend on how the stream slices it.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "sine_tone.h"

using namespace isotone::ui;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> render(SineTone& tone, double seconds, double rate) {
    std::vector<float> x(static_cast<size_t>(seconds * rate));
    tone.render(x.data(), static_cast<uint32_t>(x.size()), rate);
    return x;
}

// Upward zero crossings, interpolated between samples, as sample positions.
std::vector<double> crossings(const std::vector<float>& x) {
    std::vector<double> at;
    for (size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
    }
    return at;
}

double mean_frequency(const std::vector<float>& x, double rate) {
    const std::vector<double> at = crossings(x);
    REQUIRE(at.size() > 2);
    return rate * static_cast<double>(at.size() - 1) / (at.back() - at.front());
}

// As the stream renders it: 10 ms at a time.
void render_in_blocks(SineTone& tone, double seconds, double rate) {
    std::vector<float> b(static_cast<size_t>(rate / 100.0));
    for (size_t done = 0; done < static_cast<size_t>(seconds * 100.0); ++done) tone.render(b.data(), static_cast<uint32_t>(b.size()), rate);
}

double peak(const std::vector<float>& x, size_t from = 0) {
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i) p = std::max(p, std::abs(x[i]));
    return p;
}

}  // namespace

TEST_CASE("the tone is a sine at the level and frequency set, and silent until it is on") {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        CAPTURE(rate);
        SineTone tone;
        tone.set_frequency(3550.0);
        const std::vector<float> before = render(tone, 0.1, rate);
        CHECK(peak(before) == 0.0f);

        tone.set_on(true);
        render(tone, 0.3, rate);   // the fade in
        const std::vector<float> x = render(tone, 1.0, rate);
        // A full-scale sine is 0 dBFS, so -30 dBFS is a peak of 10^(-30/20).
        CHECK(20.0 * std::log10(peak(x)) == doctest::Approx(kEarToneDbfs).epsilon(0.0005));
        CHECK(mean_frequency(x, rate) == doctest::Approx(3550.0).epsilon(1e-5));
        CHECK(tone.frequency() == doctest::Approx(3550.0));

        tone.set_level_db(-6.0);
        tone.set_frequency(55.0);
        render(tone, 0.5, rate);
        const std::vector<float> y = render(tone, 1.0, rate);
        CHECK(20.0 * std::log10(peak(y)) == doctest::Approx(-6.0).epsilon(0.0005));
        CHECK(mean_frequency(y, rate) == doctest::Approx(55.0).epsilon(1e-5));
    }
}

TEST_CASE("the tone stays between 20 Hz and 20 kHz") {
    SineTone tone;
    tone.set_on(true);
    tone.set_frequency(5.0);
    render(tone, 0.5, 48000.0);
    CHECK(tone.frequency() == doctest::Approx(20.0));
    tone.set_frequency(30000.0);
    render(tone, 0.5, 48000.0);
    CHECK(tone.frequency() == doctest::Approx(20000.0));
}

TEST_CASE("no change to the tone steps the signal further than the sine itself moves") {
    // Frequency, level and on/off changed at random moments, in blocks of random
    // size. The sine alone moves at most A * 2 pi f / rate between two samples; the
    // level ramp adds at most its own per-sample step. A level applied at once, or
    // a phase that starts again, steps far past that.
    constexpr double rate = 48000.0;
    constexpr double lo = 60.0, hi = 400.0;
    std::mt19937 random(7);
    std::uniform_int_distribution<int> block(1, 700), what(0, 5);
    std::uniform_real_distribution<double> hz(lo, hi), db(-40.0, 0.0);

    SineTone tone;
    tone.set_frequency(hi);
    tone.set_level_db(0.0);
    tone.set_on(true);
    std::vector<float> x = render(tone, 0.2, rate);   // at full level and 400 Hz before the first change
    int changes = 0;
    while (x.size() < static_cast<size_t>(20.0 * rate)) {
        switch (what(random)) {
            case 0: tone.set_frequency(hz(random)); ++changes; break;
            case 1: tone.set_level_db(db(random)); ++changes; break;
            case 2: tone.set_on(!tone.on()); ++changes; break;
            default: break;
        }
        std::vector<float> b(static_cast<size_t>(block(random)));
        tone.render(b.data(), static_cast<uint32_t>(b.size()), rate);
        x.insert(x.end(), b.begin(), b.end());
    }
    REQUIRE(changes > 1000);

    const double ramp_step = 1.0 - std::exp(-1.0 / (SineTone::kLevelTimeConstantS * rate));
    const double bound = 2.0 * kPi * hi / rate + ramp_step;
    double worst = 0.0;
    for (size_t i = 1; i < x.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(x[i] - x[i - 1])));
    CAPTURE(worst);
    CAPTURE(bound);
    CHECK(worst <= bound * 1.001);
    CHECK(worst > bound * 0.5);   // the sine was loud and high enough to test anything
}

TEST_CASE("a frequency change glides rather than jumps") {
    constexpr double rate = 48000.0;
    SineTone tone;
    tone.set_frequency(200.0);
    tone.set_on(true);
    render(tone, 0.5, rate);
    tone.set_frequency(400.0);
    const std::vector<float> x = render(tone, 0.5, rate);
    const std::vector<double> at = crossings(x);
    REQUIRE(at.size() > 10);
    // Each period's frequency: the first after the change is still near 200 Hz, they
    // only rise, and the glide has arrived well inside half a second.
    std::vector<double> f;
    for (size_t i = 1; i < at.size(); ++i) f.push_back(rate / (at[i] - at[i - 1]));
    CAPTURE(f.front());
    CHECK(f.front() < 300.0);
    for (size_t i = 1; i < f.size(); ++i) CHECK(f[i] >= f[i - 1] - 0.01);
    CHECK(f.back() == doctest::Approx(400.0).epsilon(1e-4));
}

TEST_CASE("turning the tone off fades it to exact silence, and on fades it in") {
    constexpr double rate = 48000.0;
    const double a = std::pow(10.0, kEarToneDbfs / 20.0);
    SineTone tone;
    tone.set_frequency(1000.0);
    tone.set_on(true);
    const std::vector<float> in = render(tone, 0.3, rate);
    CHECK(peak(std::vector<float>(in.begin(), in.begin() + 48)) < 0.15 * a);   // 1 ms in: barely started
    CHECK(peak(in, in.size() - 480) == doctest::Approx(a).epsilon(1e-3));

    tone.set_on(false);
    const std::vector<float> out = render(tone, 1.0, rate);
    CHECK(peak(std::vector<float>(out.begin(), out.begin() + 48)) > 0.85 * a);   // 1 ms in: not cut
    CHECK(peak(std::vector<float>(out.begin() + 4800, out.begin() + 9600)) < 1e-3 * a);   // 100 ms on
    const std::vector<float> after = render(tone, 0.1, rate);
    CHECK(peak(after) == 0.0f);   // exact zeros, not a tail decaying into denormals
}

TEST_CASE("how the stream slices the render does not change the samples") {
    constexpr double rate = 44100.0;
    SineTone whole, sliced;
    for (SineTone* t : {&whole, &sliced}) {
        t->set_frequency(137.0);
        t->set_level_db(-12.0);
        t->set_on(true);
        t->set_sweep(0.7);
    }
    const std::vector<float> a = render(whole, 3.0, rate);
    std::vector<float> b;
    std::mt19937 random(3);
    std::uniform_int_distribution<int> block(1, 2000);
    while (b.size() < a.size()) {
        std::vector<float> part(std::min(static_cast<size_t>(block(random)), a.size() - b.size()));
        sliced.render(part.data(), static_cast<uint32_t>(part.size()), rate);
        b.insert(b.end(), part.begin(), part.end());
    }
    CHECK(a == b);
}

TEST_CASE("the auto sweep moves at its rate in octaves and turns at either end") {
    constexpr double rate = 48000.0;
    SineTone tone;
    tone.set_on(true);

    SUBCASE("up one octave in a second") {
        tone.set_frequency(1000.0);
        render(tone, 0.2, rate);
        tone.set_sweep(1.0);
        render(tone, 1.0, rate);
        CHECK(tone.frequency() == doctest::Approx(2000.0).epsilon(1e-3));
        tone.set_sweep(0.0);
        const std::vector<float> held = render(tone, 1.0, rate);
        CHECK(tone.frequency() == doctest::Approx(2000.0).epsilon(1e-3));
        CHECK(mean_frequency(held, rate) == doctest::Approx(2000.0).epsilon(1e-3));
    }
    SUBCASE("turns down at 20 kHz") {
        tone.set_frequency(16000.0);
        render(tone, 0.2, rate);
        tone.set_sweep(1.0);
        render_in_blocks(tone, 1.0, rate);
        // log2(16000) + 1 goes past log2(20000) and comes back as far: 20000^2 / 32000.
        CHECK(tone.frequency() == doctest::Approx(12500.0).epsilon(1e-3));
    }
    SUBCASE("a negative rate sweeps down and turns up at 20 Hz") {
        tone.set_frequency(25.0);
        render(tone, 0.2, rate);
        tone.set_sweep(-1.0);
        render_in_blocks(tone, 1.0, rate);
        CHECK(tone.frequency() == doctest::Approx(32.0).epsilon(1e-3));
    }
    SUBCASE("setting the frequency while it sweeps moves it there, and it sweeps on from there") {
        tone.set_frequency(1000.0);
        render(tone, 0.2, rate);
        tone.set_sweep(1.0);
        render(tone, 0.5, rate);
        tone.set_frequency(100.0);
        render(tone, 1.0, rate);
        // The glide to 100 Hz lags the sweep by its time constant; a second later
        // the sweep has carried it an octave on from there.
        CHECK(tone.frequency() == doctest::Approx(200.0).epsilon(0.03));
    }
}
