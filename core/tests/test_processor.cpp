// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The audio path. The important test here is measured_response_matches_curve:
// it plays sine tones through the actual filters and checks the result against
// the analytic curve response.cpp computes. Those are two entirely separate
// pieces of code, and the plan's promise is that the curve drawn is the curve
// heard, so they have to agree.

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "isotone/processor.h"
#include "isotone/response.h"

using namespace isotone;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

Band peaking(double fc, double gain_db, double q, ChannelMask ch = kAllChannels) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    b.channels = ch;
    return b;
}

// Single-bin DFT. Choosing freq = bin * fs / n makes the window an exact whole
// number of cycles, so this measures amplitude with no spectral leakage and no
// window function needed.
double amplitude_at_bin(const std::vector<float>& x, uint32_t bin) {
    const size_t n = x.size();
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 2.0 * kPi * bin * static_cast<double>(i) / static_cast<double>(n);
        re += x[i] * std::cos(w);
        im -= x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

// Plays a unit sine at bin*fs/n through the processor and returns the gain in dB
// of the channel requested. `settle` frames are processed and discarded first so
// the IIR transient is gone before measurement.
double measure_gain_db(Processor& p, uint32_t bin, size_t n, uint32_t channel,
                       size_t settle = 8192) {
    const uint32_t ch = p.channels();
    std::vector<std::vector<float>> buf(ch);
    std::vector<float*> ptr(ch);

    auto fill = [&](size_t count, size_t phase_offset) {
        for (uint32_t c = 0; c < ch; ++c) {
            buf[c].resize(count);
            for (size_t i = 0; i < count; ++i) {
                const double t = static_cast<double>(i + phase_offset);
                buf[c][i] = static_cast<float>(
                    std::sin(2.0 * kPi * bin * t / static_cast<double>(n)));
            }
            ptr[c] = buf[c].data();
        }
    };

    fill(settle, 0);
    p.process(ptr.data(), static_cast<uint32_t>(settle));
    fill(n, settle);
    p.process(ptr.data(), static_cast<uint32_t>(n));

    return 20.0 * std::log10(amplitude_at_bin(buf[channel], bin));
}

double bin_to_hz(uint32_t bin, size_t n) {
    return static_cast<double>(bin) * kFs / static_cast<double>(n);
}

Processor make(const EqState& s, uint32_t channels = 2, uint32_t max_frames = 8192) {
    Processor p;
    p.initialize(kFs, channels, max_frames);
    p.set_target(s);
    p.reset();
    return p;
}

}  // namespace

TEST_CASE("an empty state passes audio through unchanged") {
    EqState s;
    Processor p = make(s);

    std::vector<float> l(512), r(512);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = dist(rng);
        r[i] = dist(rng);
    }
    const std::vector<float> l0 = l, r0 = r;

    float* ptr[2] = {l.data(), r.data()};
    p.process(ptr, 512);

    for (size_t i = 0; i < l.size(); ++i) {
        CHECK(l[i] == doctest::Approx(l0[i]).epsilon(1e-6));
        CHECK(r[i] == doctest::Approx(r0[i]).epsilon(1e-6));
    }
}

TEST_CASE("measured response matches the analytic curve") {
    EqState s;
    s.bands.push_back(peaking(1000.0, -12.0, 1.0));
    s.bands.push_back(peaking(250.0, 6.0, 0.7));
    s.bands.push_back(peaking(6000.0, 4.0, 3.0));

    Band ls;
    ls.type = FilterType::LowShelf;
    ls.fc = 120.0;
    ls.gain_db = 5.0;
    ls.width = 0.70;
    s.bands.push_back(ls);

    Band hp;
    hp.type = FilterType::HighPass;
    hp.fc = 40.0;
    hp.width = 0.7071;
    s.bands.push_back(hp);

    constexpr size_t kN = 16384;

    // Bins chosen to land near musically interesting frequencies. Each is an
    // exact whole number of cycles in the window.
    const uint32_t bins[] = {14, 27, 41, 68, 85, 137, 205, 341, 512, 683,
                             1024, 1365, 2048, 2731, 4096, 5461};

    for (uint32_t bin : bins) {
        const double hz = bin_to_hz(bin, kN);
        Processor p = make(s, 2, static_cast<uint32_t>(kN));
        const double measured = measure_gain_db(p, bin, kN, 0);

        double expected = 0.0;
        magnitude_db(s, 0, &hz, 1, kFs, &expected);

        CAPTURE(bin);
        CAPTURE(hz);
        CAPTURE(measured);
        CAPTURE(expected);
        CHECK(measured == doctest::Approx(expected).epsilon(0.0).scale(1.0).epsilon(1e-3));
    }
}

TEST_CASE("preamp, channel trim and mute are applied in the right order") {
    constexpr size_t kN = 8192;
    const uint32_t bin = 171;  // about 1002 Hz

    EqState s;
    s.preamp_db = -6.0;
    {
        Processor p = make(s, 2, static_cast<uint32_t>(kN));
        CHECK(measure_gain_db(p, bin, kN, 0) == doctest::Approx(-6.0).epsilon(1e-4));
    }

    s.channel_gain_db[1] = -3.0;
    {
        Processor p = make(s, 2, static_cast<uint32_t>(kN));
        CHECK(measure_gain_db(p, bin, kN, 0) == doctest::Approx(-6.0).epsilon(1e-4));
        CHECK(measure_gain_db(p, bin, kN, 1) == doctest::Approx(-9.0).epsilon(1e-4));
    }

    s.mute = true;
    {
        Processor p = make(s, 2, static_cast<uint32_t>(kN));
        CHECK(measure_gain_db(p, bin, kN, 0) < -100.0);
    }
}

TEST_CASE("mono downmix averages the channels") {
    EqState s;
    s.mono = true;
    Processor p = make(s);

    std::vector<float> l(64, 1.0f), r(64, -0.5f);
    float* ptr[2] = {l.data(), r.data()};
    p.process(ptr, 64);

    for (size_t i = 0; i < 64; ++i) {
        CHECK(l[i] == doctest::Approx(0.25f).epsilon(1e-5));
        CHECK(r[i] == doctest::Approx(0.25f).epsilon(1e-5));
    }
}

TEST_CASE("bypass passes the dry signal") {
    EqState s;
    s.bands.push_back(peaking(1000.0, -24.0, 1.0));
    s.bypass = true;
    Processor p = make(s);

    std::vector<float> l(256), r(256);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
        r[i] = l[i];
    }
    const std::vector<float> l0 = l;

    float* ptr[2] = {l.data(), r.data()};
    p.process(ptr, 256);

    for (size_t i = 0; i < l.size(); ++i) {
        CHECK(l[i] == doctest::Approx(l0[i]).epsilon(1e-5));
    }
}

TEST_CASE("channel masks route bands to the right channel in the audio path") {
    constexpr size_t kN = 8192;
    const uint32_t bin = 171;

    EqState s;
    s.bands.push_back(peaking(bin_to_hz(bin, kN), -12.0, 4.0, 1u << 0));

    Processor p = make(s, 2, static_cast<uint32_t>(kN));
    const double left  = measure_gain_db(p, bin, kN, 0);
    const double right = measure_gain_db(p, bin, kN, 1);

    CHECK(left < -11.0);
    CHECK(right == doctest::Approx(0.0).epsilon(1e-4));
}

namespace {

// Runs an interleaved stream of `channels` sines, channel c at amplitude
// (c + 1) / (channels + 4), and returns each channel's level in dB relative to
// its own input amplitude. Distinct amplitudes make a channel read from the
// wrong slot show up as a gain error.
std::vector<double> interleaved_gains_db(const EqState& s, uint32_t channels, uint32_t bin,
                                         size_t n) {
    Processor p;
    p.initialize(kFs, channels, static_cast<uint32_t>(n));
    p.set_target(s);
    p.reset();

    const auto amp = [&](uint32_t c) { return (c + 1.0) / (channels + 4.0); };
    std::vector<float> buf(n * channels);
    auto fill = [&](size_t offset) {
        for (size_t i = 0; i < n; ++i) {
            const double v = std::sin(2.0 * kPi * bin * static_cast<double>(i + offset) /
                                      static_cast<double>(n));
            for (uint32_t c = 0; c < channels; ++c) {
                buf[i * channels + c] = static_cast<float>(amp(c) * v);
            }
        }
    };
    fill(0);
    p.process_interleaved(buf.data(), static_cast<uint32_t>(n));   // settle
    fill(n);
    p.process_interleaved(buf.data(), static_cast<uint32_t>(n));

    std::vector<double> gains(channels);
    std::vector<float> one(n);
    for (uint32_t c = 0; c < channels; ++c) {
        for (size_t i = 0; i < n; ++i) one[i] = buf[i * channels + c];
        gains[c] = 20.0 * std::log10(amplitude_at_bin(one, bin) / amp(c));
    }
    return gains;
}

}  // namespace

TEST_CASE("an interleaved stream wider than the trim table is processed channel by channel") {
    // 7.1.4 is twelve channels. The processor must walk the buffer with the
    // stream's real stride and filter every channel, not just the ones that can
    // carry a trim in the param block.
    constexpr size_t kN = 8192;
    const uint32_t bin = 171;
    const double hz = bin_to_hz(bin, kN);

    EqState s;
    s.bands.push_back(peaking(hz, -12.0, 4.0, ChannelMask{1} << 10));
    s.bands.push_back(peaking(hz, -3.0, 4.0));   // every channel
    s.channel_gain_db[3] = -6.0;

    const std::vector<double> gains = interleaved_gains_db(s, 12, bin, kN);
    for (uint32_t c = 0; c < 12; ++c) {
        double expected = 0.0;
        magnitude_db(s, c, &hz, 1, kFs, &expected);
        CAPTURE(c);
        CHECK(gains[c] == doctest::Approx(expected).epsilon(0.0).scale(1.0).epsilon(1e-3));
    }
    CHECK(gains[10] == doctest::Approx(-15.0).epsilon(1e-3));
    CHECK(gains[3] == doctest::Approx(-9.0).epsilon(1e-3));
    CHECK(gains[11] == doctest::Approx(-3.0).epsilon(1e-3));
}

TEST_CASE("channels past the mask width are reached only by all-channel bands") {
    // A ChannelMask names channels 0 to 31. On x86 a 32-bit shift by 33 wraps
    // to a shift by 1, so a careless mask test would put channel 1's band on
    // channel 33 as well.
    constexpr size_t kN = 8192;
    const uint32_t bin = 171;
    const double hz = bin_to_hz(bin, kN);

    EqState s;
    s.bands.push_back(peaking(hz, -12.0, 4.0, ChannelMask{1} << 1));

    const std::vector<double> gains = interleaved_gains_db(s, 34, bin, kN);
    CHECK(gains[1] == doctest::Approx(-12.0).epsilon(1e-3));
    CHECK(gains[33] == doctest::Approx(0.0).epsilon(1e-3));

    double curve = 1.0;
    magnitude_db(s, 33, &hz, 1, kFs, &curve);
    CHECK(curve == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("a gain ramp during a sine produces no click") {
    // The hard requirement from plan 4.4. A sine plays while gain sweeps from
    // 0 to -12 dB over 100 ms. The output is checked sample by sample: a click
    // is a step change, so it shows up as a sample-to-sample difference far
    // larger than a sine of that amplitude can produce on its own.
    constexpr double kFreq = 1000.0;
    constexpr uint32_t kBlock = 64;
    const uint32_t total = static_cast<uint32_t>(kFs * 0.3);

    EqState s;
    s.bands.push_back(peaking(kFreq, 0.0, 1.0));

    Processor p;
    p.initialize(kFs, 2, kBlock);
    p.set_target(s);
    p.reset();

    std::vector<float> out;
    out.reserve(total);

    std::vector<float> l(kBlock), r(kBlock);
    float* ptr[2] = {l.data(), r.data()};

    const uint32_t ramp_start = static_cast<uint32_t>(kFs * 0.05);
    const uint32_t ramp_len   = static_cast<uint32_t>(kFs * 0.10);

    for (uint32_t pos = 0; pos < total; pos += kBlock) {
        const double frac = std::clamp(
            (static_cast<double>(pos) - ramp_start) / static_cast<double>(ramp_len), 0.0, 1.0);
        s.bands[0].gain_db = -12.0 * frac;
        p.set_target(s);

        for (uint32_t i = 0; i < kBlock; ++i) {
            const double t = static_cast<double>(pos + i);
            l[i] = r[i] = static_cast<float>(std::sin(2.0 * kPi * kFreq * t / kFs));
        }
        p.process(ptr, kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }

    // A unit sine at 1 kHz and 48 kHz can move at most 2*sin(pi*f/fs) = 0.1308
    // between adjacent samples. Anything appreciably above that is a step.
    const double sine_max_delta = 2.0 * std::sin(kPi * kFreq / kFs);
    double worst = 0.0;
    size_t worst_i = 0;
    for (size_t i = 1; i < out.size(); ++i) {
        const double d = std::abs(static_cast<double>(out[i]) - static_cast<double>(out[i - 1]));
        if (d > worst) {
            worst = d;
            worst_i = i;
        }
    }
    CAPTURE(sine_max_delta);
    CAPTURE(worst);
    CAPTURE(worst_i);
    CHECK(worst < sine_max_delta * 1.10);

    // And prove the test is not vacuous: the gain really did change.
    const double before = *std::max_element(out.begin(), out.begin() + ramp_start);
    const double after  = *std::max_element(out.end() - 4096, out.end());
    CHECK(before == doctest::Approx(1.0).epsilon(0.02));
    CHECK(after  == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.05));
}

TEST_CASE("a filter type change crossfades instead of clicking") {
    constexpr double kFreq = 1000.0;
    constexpr uint32_t kBlock = 64;
    const uint32_t total = static_cast<uint32_t>(kFs * 0.2);
    const uint32_t switch_at = static_cast<uint32_t>(kFs * 0.1);

    EqState s;
    s.bands.push_back(peaking(kFreq, 9.0, 1.0));

    Processor p;
    p.initialize(kFs, 2, kBlock);
    p.set_target(s);
    p.reset();

    std::vector<float> out;
    std::vector<float> l(kBlock), r(kBlock);
    float* ptr[2] = {l.data(), r.data()};

    for (uint32_t pos = 0; pos < total; pos += kBlock) {
        if (pos >= switch_at && s.bands[0].type == FilterType::Peaking) {
            s.bands[0].type = FilterType::Notch;
            p.set_target(s);
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            const double t = static_cast<double>(pos + i);
            l[i] = r[i] = static_cast<float>(std::sin(2.0 * kPi * kFreq * t / kFs));
        }
        p.process(ptr, kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }

    // Peaking at +9 dB is an amplitude of 2.82, so the sine can legitimately
    // move 2.82 * 0.1308 per sample. Allow that plus a small margin.
    const double peak_amp = std::pow(10.0, 9.0 / 20.0);
    const double limit = peak_amp * 2.0 * std::sin(kPi * kFreq / kFs) * 1.15;
    double worst = 0.0;
    for (size_t i = 1; i < out.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(out[i]) -
                                         static_cast<double>(out[i - 1])));
    }
    CAPTURE(limit);
    CAPTURE(worst);
    CHECK(worst < limit);

    // The notch really did take hold.
    const double tail = *std::max_element(out.end() - 2048, out.end());
    CHECK(tail < 0.05);
}

TEST_CASE("smoothed parameters converge to their target") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 0.0, 1.0));
    Processor p;
    p.initialize(kFs, 2, 512);
    p.set_target(s);
    p.reset();

    s.bands[0].gain_db = -12.0;
    s.bands[0].fc = 4000.0;
    p.set_target(s);
    CHECK(p.is_settling());

    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    float* ptr[2] = {l.data(), r.data()};
    // 20 ms time constant, so 300 ms is fifteen time constants.
    for (int i = 0; i < 30; ++i) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        p.process(ptr, 512);
    }
    CHECK_FALSE(p.is_settling());

    constexpr size_t kN = 8192;
    Processor q = make(s, 2, static_cast<uint32_t>(kN));
    const uint32_t bin = 683;  // about 4002 Hz
    const double hz = bin_to_hz(bin, kN);
    double expected = 0.0;
    magnitude_db(s, 0, &hz, 1, kFs, &expected);
    CHECK(measure_gain_db(q, bin, kN, 0) == doctest::Approx(expected).epsilon(1e-3));
}

TEST_CASE("output is independent of block size") {
    EqState s;
    s.bands.push_back(peaking(800.0, 6.0, 2.0));
    s.bands.push_back(peaking(3000.0, -9.0, 0.8));

    auto run = [&](uint32_t block) {
        Processor p;
        p.initialize(kFs, 2, 8192);
        p.set_target(s);
        p.reset();
        std::vector<float> l(4096), r(4096);
        for (size_t i = 0; i < l.size(); ++i) {
            l[i] = r[i] = static_cast<float>(
                std::sin(2.0 * kPi * 700.0 * static_cast<double>(i) / kFs));
        }
        for (uint32_t pos = 0; pos < 4096; pos += block) {
            const uint32_t n = std::min(block, 4096u - pos);
            float* ptr[2] = {l.data() + pos, r.data() + pos};
            p.process(ptr, n);
        }
        return l;
    };

    const std::vector<float> a = run(1);
    const std::vector<float> b = run(64);
    const std::vector<float> c = run(4096);
    for (size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-5));
        CHECK(a[i] == doctest::Approx(c[i]).epsilon(1e-5));
    }
}

TEST_CASE("interleaved and planar produce the same output") {
    EqState s;
    s.bands.push_back(peaking(1200.0, -6.0, 1.5));

    std::vector<float> l(1024), r(1024), inter(2048);
    for (size_t i = 0; i < l.size(); ++i) {
        const float v = static_cast<float>(
            std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
        l[i] = v;
        r[i] = v * 0.5f;
        inter[i * 2 + 0] = l[i];
        inter[i * 2 + 1] = r[i];
    }

    Processor p = make(s, 2, 1024);
    float* ptr[2] = {l.data(), r.data()};
    p.process(ptr, 1024);

    Processor q = make(s, 2, 1024);
    q.process_interleaved(inter.data(), 1024);

    for (size_t i = 0; i < 1024; ++i) {
        CAPTURE(i);
        CHECK(inter[i * 2 + 0] == doctest::Approx(l[i]).epsilon(1e-6));
        CHECK(inter[i * 2 + 1] == doctest::Approx(r[i]).epsilon(1e-6));
    }
}

TEST_CASE("output stays bounded under random parameter jumps") {
    // Stability fuzz from plan stage 2: slam the parameters around while white
    // noise plays and assert nothing runs away.
    std::mt19937 rng(20260912);
    std::uniform_real_distribution<double> fc(20.0, 20000.0);
    std::uniform_real_distribution<double> gain(-24.0, 24.0);
    std::uniform_real_distribution<double> q(0.1, 24.0);
    std::uniform_int_distribution<int> type(0, 7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    EqState s;
    for (int i = 0; i < 8; ++i) {
        s.bands.push_back(peaking(1000.0, 0.0, 1.0));
    }

    Processor p;
    p.initialize(kFs, 2, 256);
    p.set_target(s);
    p.reset();

    std::vector<float> l(256), r(256);
    float* ptr[2] = {l.data(), r.data()};

    const int blocks = static_cast<int>(kFs * 10.0 / 256.0);  // 10 seconds
    for (int b = 0; b < blocks; ++b) {
        for (Band& band : s.bands) {
            band.fc = fc(rng);
            band.gain_db = gain(rng);
            band.width = q(rng);
            band.type = static_cast<FilterType>(type(rng));
        }
        p.set_target(s);

        for (size_t i = 0; i < l.size(); ++i) {
            l[i] = noise(rng);
            r[i] = noise(rng);
        }
        p.process(ptr, 256);

        for (size_t i = 0; i < l.size(); ++i) {
            REQUIRE(std::isfinite(l[i]));
            REQUIRE(std::isfinite(r[i]));
            REQUIRE(std::abs(l[i]) < 1000.0f);
            REQUIRE(std::abs(r[i]) < 1000.0f);
        }
    }
}

TEST_CASE("a band appearing or disappearing does not click") {
    constexpr double kFreq = 1000.0;
    constexpr uint32_t kBlock = 64;
    const uint32_t total = static_cast<uint32_t>(kFs * 0.3);

    EqState s;
    Processor p;
    p.initialize(kFs, 2, kBlock);
    p.set_target(s);
    p.reset();

    std::vector<float> out;
    std::vector<float> l(kBlock), r(kBlock);
    float* ptr[2] = {l.data(), r.data()};

    const uint32_t add_at    = static_cast<uint32_t>(kFs * 0.10);
    const uint32_t remove_at = static_cast<uint32_t>(kFs * 0.20);

    for (uint32_t pos = 0; pos < total; pos += kBlock) {
        if (pos >= add_at && pos < remove_at && s.bands.empty()) {
            s.bands.push_back(peaking(kFreq, -18.0, 2.0));
            p.set_target(s);
        } else if (pos >= remove_at && !s.bands.empty()) {
            s.bands.clear();
            p.set_target(s);
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            const double t = static_cast<double>(pos + i);
            l[i] = r[i] = static_cast<float>(std::sin(2.0 * kPi * kFreq * t / kFs));
        }
        p.process(ptr, kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }

    const double limit = 2.0 * std::sin(kPi * kFreq / kFs) * 1.10;
    double worst = 0.0;
    for (size_t i = 1; i < out.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(out[i]) -
                                         static_cast<double>(out[i - 1])));
    }
    CAPTURE(worst);
    CHECK(worst < limit);
}

TEST_CASE("control block cadence scales with sample rate") {
    CHECK(control_block_frames(48000.0) == 32);
    CHECK(control_block_frames(44100.0) == 29);
    CHECK(control_block_frames(96000.0) == 64);
    CHECK(control_block_frames(192000.0) == 128);
    CHECK(control_block_frames(0.0) == 32);
}

TEST_CASE("processing runs at every supported sample rate") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        EqState s;
        s.bands.push_back(peaking(1000.0, -12.0, 1.0));

        Processor p;
        p.initialize(fs, 2, 1024);
        p.set_target(s);
        p.reset();

        std::vector<float> l(1024), r(1024);
        for (size_t i = 0; i < l.size(); ++i) {
            l[i] = r[i] = static_cast<float>(
                std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / fs));
        }
        float* ptr[2] = {l.data(), r.data()};
        p.process(ptr, 1024);

        CAPTURE(fs);
        for (size_t i = 0; i < l.size(); ++i) {
            REQUIRE(std::isfinite(l[i]));
        }
        // Well past the transient, the -12 dB dip should be visible.
        const double tail = *std::max_element(l.begin() + 512, l.end());
        CHECK(tail < 0.30);
    }
}

TEST_CASE("silence after a decaying impulse does not cost extra time") {
    // Denormal check from plan stage 2. A filter ringing down produces
    // ever-smaller numbers; without flush-to-zero those become denormals and the
    // CPU falls off a cliff. Both runs process the same count of samples, so a
    // large ratio means denormals are being handled in microcode.
    enable_denormal_flushing();

    EqState s;
    s.bands.push_back(peaking(100.0, 24.0, 30.0));

    auto run_ns = [&](bool impulse) {
        Processor p;
        p.initialize(kFs, 2, 4096);
        p.set_target(s);
        p.reset();
        std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
        if (impulse) {
            l[0] = r[0] = 1.0f;
        }
        float* ptr[2] = {l.data(), r.data()};
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) {
            p.process(ptr, 4096);
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            if (impulse && i == 0) {
                // only the very first block carried the impulse
            }
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start).count();
    };

    const auto quiet = run_ns(false);
    const auto rung  = run_ns(true);
    CAPTURE(quiet);
    CAPTURE(rung);
    CHECK(rung < quiet * 4);
}
