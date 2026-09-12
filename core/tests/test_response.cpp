// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Composite response: how bands, preamp, trims, channel masks and the global
// flags combine into the curve the UI draws.

#include "doctest.h"

#include <cmath>
#include <vector>

#include "isotone/biquad.h"
#include "isotone/response.h"

using namespace isotone;

namespace {

constexpr double kFs = 48000.0;

Band peaking(double fc, double gain_db, double q, ChannelMask ch = kAllChannels) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    b.channels = ch;
    return b;
}

double mag_at(const EqState& s, uint32_t channel, double freq) {
    double out = 0.0;
    magnitude_db(s, channel, &freq, 1, kFs, &out);
    return out;
}

}  // namespace

TEST_CASE("empty state is flat at 0 dB") {
    EqState s;
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0));
    CHECK(mag_at(s, 0, 20.0) == doctest::Approx(0.0));
}

TEST_CASE("overlapping bands add in dB") {
    // Two identical +4 dB bells at the same frequency must read +8 dB, which is
    // the behaviour auto-preamp exists to compensate for (plan 4.6).
    EqState s;
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(8.0).epsilon(1e-9));
}

TEST_CASE("preamp and channel trim shift the whole curve") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0));
    s.preamp_db = -6.0;
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0).epsilon(1e-9));
    // At 20 Hz the Q=1 bell at 1 kHz still contributes a few thousandths of a dB
    // of skirt, so this is preamp plus that, not preamp alone.
    CHECK(mag_at(s, 0, 20.0) == doctest::Approx(-6.0).epsilon(1e-3));

    s.channel_gain_db[1] = -3.0;
    CHECK(mag_at(s, 1, 1000.0) == doctest::Approx(-3.0).epsilon(1e-9));
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("bypass flattens the curve and ignores everything else") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    s.preamp_db = -20.0;
    s.mute = true;
    s.bypass = true;
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0));
}

TEST_CASE("mute drives the curve to negative infinity") {
    EqState s;
    s.mute = true;
    CHECK(std::isinf(mag_at(s, 0, 1000.0)));
    CHECK(mag_at(s, 0, 1000.0) < 0.0);
}

TEST_CASE("channel masks select which bands apply") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0, 1u << 0));  // left only
    s.bands.push_back(peaking(4000.0, -6.0, 1.0, 1u << 1)); // right only

    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(6.0).epsilon(1e-9));
    CHECK(mag_at(s, 1, 4000.0) == doctest::Approx(-6.0).epsilon(1e-9));

    // Away from its own centre each channel should show exactly the skirt of its
    // own band and nothing of the other channel's. A Q=1 bell two octaves away
    // is still worth about 0.4 dB, so compare against the band itself rather
    // than against zero.
    double f = 4000.0, own = 0.0;
    band_magnitude_db(s.bands[0], &f, 1, kFs, &own);
    CHECK(mag_at(s, 0, 4000.0) == doctest::Approx(own).epsilon(1e-12));
    f = 1000.0;
    band_magnitude_db(s.bands[1], &f, 1, kFs, &own);
    CHECK(mag_at(s, 1, 1000.0) == doctest::Approx(own).epsilon(1e-12));
}

TEST_CASE("mask 0 means every channel") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0, kAllChannels));
    for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        CAPTURE(ch);
        CHECK(mag_at(s, ch, 1000.0) == doctest::Approx(6.0).epsilon(1e-9));
    }
}

TEST_CASE("disabled bands contribute nothing") {
    EqState s;
    Band b = peaking(1000.0, 12.0, 1.0);
    b.enabled = false;
    s.bands.push_back(b);
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0));
}

TEST_CASE("band_magnitude_db ignores preamp, trims and mute") {
    EqState s;
    const Band b = peaking(1000.0, 6.0, 1.0);
    s.bands.push_back(b);
    s.preamp_db = -20.0;
    s.mute = true;

    double freq = 1000.0, out = 0.0;
    band_magnitude_db(b, &freq, 1, kFs, &out);
    CHECK(out == doctest::Approx(6.0).epsilon(1e-9));
}

TEST_CASE("composite_peak_db finds a narrow peak between grid points") {
    // A Q=30 bell at 3333 Hz is narrow enough to fall between 256 log-spaced
    // points. The band-centre sampling in composite_peak_db is what catches it.
    EqState s;
    s.bands.push_back(peaking(3333.0, 9.0, 30.0));
    const std::vector<double> grid = log_grid(10.0, kFs * 0.5 * 0.95, 256);
    const double peak = composite_peak_db(s, 2, grid.data(), grid.size(), kFs);
    CHECK(peak == doctest::Approx(9.0).epsilon(1e-6));
}

TEST_CASE("composite_peak_db accounts for summed bands and channel trims") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    const std::vector<double> grid = log_grid(10.0, 22800.0, 512);
    CHECK(composite_peak_db(s, 2, grid.data(), grid.size(), kFs) ==
          doctest::Approx(8.0).epsilon(1e-6));

    s.channel_gain_db[1] = 3.0;
    CHECK(composite_peak_db(s, 2, grid.data(), grid.size(), kFs) ==
          doctest::Approx(11.0).epsilon(1e-6));
}

TEST_CASE("composite_peak_db is zero for a flat or bypassed state") {
    const std::vector<double> grid = log_grid(10.0, 22800.0, 128);
    EqState s;
    CHECK(composite_peak_db(s, 2, grid.data(), grid.size(), kFs) == doctest::Approx(0.0));

    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    s.bypass = true;
    CHECK(composite_peak_db(s, 2, grid.data(), grid.size(), kFs) == doctest::Approx(0.0));
}

TEST_CASE("phase of a cascade is the sum of the parts") {
    EqState s;
    s.bands.push_back(peaking(500.0, 6.0, 1.0));
    s.bands.push_back(peaking(4000.0, -6.0, 2.0));

    double freq = 1500.0;
    double composite = 0.0;
    phase_deg(s, 0, &freq, 1, kFs, &composite);

    double sum = 0.0;
    for (const Band& b : s.bands) {
        sum += isotone::phase_deg(design(b, kFs), freq, kFs);
    }
    double diff = std::abs(composite - sum);
    if (diff > 180.0) diff = 360.0 - diff;
    CHECK(diff < 1e-9);
}

TEST_CASE("log_grid is logarithmically spaced and hits both endpoints") {
    const std::vector<double> g = log_grid(10.0, 20000.0, 256);
    REQUIRE(g.size() == 256);
    CHECK(g.front() == doctest::Approx(10.0));
    CHECK(g.back() == doctest::Approx(20000.0));
    const double ratio = g[1] / g[0];
    for (size_t i = 1; i < g.size(); ++i) {
        CAPTURE(i);
        CHECK(g[i] / g[i - 1] == doctest::Approx(ratio).epsilon(1e-9));
    }
}

TEST_CASE("log_grid rejects nonsense ranges") {
    CHECK(log_grid(0.0, 100.0, 16).empty());
    CHECK(log_grid(100.0, 10.0, 16).empty());
    CHECK(log_grid(10.0, 100.0, 0).empty());
}

TEST_CASE("a realistic AutoEq-shaped preset evaluates sanely") {
    // Sennheiser HD 650 / oratory1990, the sample quoted in plan 4.2.
    EqState s;
    s.preamp_db = -6.1;

    Band lsc;
    lsc.type = FilterType::LowShelf;
    lsc.fc = 105.0;
    lsc.gain_db = 6.4;
    lsc.width = 0.70;
    s.bands.push_back(lsc);

    s.bands.push_back(peaking(8800.0, 5.1, 1.42));
    s.bands.push_back(peaking(118.0, -3.1, 0.50));

    Band hsc;
    hsc.type = FilterType::HighShelf;
    hsc.fc = 10000.0;
    hsc.gain_db = -2.1;
    hsc.width = 0.70;
    s.bands.push_back(hsc);

    const std::vector<double> grid = log_grid(10.0, 22800.0, 512);
    std::vector<double> out(grid.size());
    magnitude_db(s, 0, grid.data(), grid.size(), kFs, out.data());

    for (double v : out) {
        CHECK(std::isfinite(v));
        CHECK(v > -40.0);
        CHECK(v < 20.0);
    }
    // Preamp of -6.1 dB against a composite peak: the result should not clip far
    // above 0 dB anywhere.
    const double peak = composite_peak_db(s, 2, grid.data(), grid.size(), kFs);
    CHECK(peak + s.preamp_db < 1.0);
}
