// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Composite response: how bands, preamp, trims, channel masks and the global
// flags combine into the curve the UI draws.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
    magnitude_db(s, kMaxChannels, 0, channel, &freq, 1, kFs, &out);
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

TEST_CASE("bypass removes the bands and preamp from the curve, not trims or mute") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    s.preamp_db = -20.0;
    s.channel_gain_db[1] = -3.0;
    s.bypass = true;
    CHECK(mag_at(s, 0, 1000.0) == doctest::Approx(0.0));
    CHECK(mag_at(s, 1, 1000.0) == doctest::Approx(-3.0));
    s.mute = true;
    CHECK(std::isinf(mag_at(s, 0, 1000.0)));
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
    const double peak = composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs);
    CHECK(peak == doctest::Approx(9.0).epsilon(1e-6));
}

TEST_CASE("composite_peak_db accounts for summed bands and channel trims") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    s.bands.push_back(peaking(1000.0, 4.0, 1.0));
    const std::vector<double> grid = log_grid(10.0, 22800.0, 512);
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) ==
          doctest::Approx(8.0).epsilon(1e-6));

    s.channel_gain_db[1] = 3.0;
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) ==
          doctest::Approx(11.0).epsilon(1e-6));
}

TEST_CASE("composite_peak_db looks at every channel of a wide layout") {
    // Auto preamp has to protect channel 9 of a 7.1.4 layout as much as channel
    // 0, even though only the first kMaxChannels channels can carry a trim.
    EqState s;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0, ChannelMask{1} << 9));
    const std::vector<double> grid = log_grid(10.0, 22800.0, 512);
    CHECK(composite_peak_db(s, 12, 0x2D63F, grid.data(), grid.size(), kFs) ==
          doctest::Approx(6.0).epsilon(1e-6));
}

TEST_CASE("composite_peak_db is zero for a flat or bypassed state") {
    const std::vector<double> grid = log_grid(10.0, 22800.0, 128);
    EqState s;
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(0.0));

    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    s.bypass = true;
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(0.0));
    // Bypass keeps the trims, so their headroom is still needed.
    s.channel_gain_db[0] = 4.0;
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(4.0));
}

TEST_CASE("auto preamp only cuts") {
    const std::vector<double> grid = log_grid(10.0, 22800.0, 256);
    EqState s;
    for (double& trim : s.channel_gain_db) trim = -6.0;
    // The peak is -6 dB; auto does not boost to meet 0 dB.
    CHECK(composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(-6.0));
    CHECK(auto_preamp_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == 0.0);

    for (double& trim : s.channel_gain_db) trim = 0.0;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0));
    CHECK(auto_preamp_db(s, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(-6.0).epsilon(1e-9));

    // A narrow peak between grid points, and a wide layout's ninth channel.
    EqState narrow;
    narrow.bands.push_back(peaking(3333.0, 9.0, 30.0));
    CHECK(auto_preamp_db(narrow, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(-9.0).epsilon(1e-6));
    EqState wide;
    wide.bands.push_back(peaking(1000.0, 6.0, 1.0, ChannelMask{1} << 9));
    CHECK(auto_preamp_db(wide, 12, 0x2D63F, grid.data(), grid.size(), kFs) == doctest::Approx(-6.0).epsilon(1e-6));
}

TEST_CASE("a state written for another layout is drawn where the engine plays it") {
    // Written for 7.1 (SL is channel 6), drawn for 5.1 surround (SL is channel 4),
    // where IsoAPO and the Equalizer APO backend both move it.
    EqState s;
    s.layout_channels = 8;
    s.layout_speaker_mask = 0x63F;
    s.bands.push_back(peaking(1000.0, 6.0, 1.0, ChannelMask{1} << 6));
    s.channel_gain_db[6] = -3.0;
    const double f = 1000.0;
    double out = 0.0;
    magnitude_db(s, 6, 0x60F, 4, &f, 1, kFs, &out);
    CHECK(out == doctest::Approx(3.0).epsilon(1e-9));
    magnitude_db(s, 6, 0x60F, 0, &f, 1, kFs, &out);
    CHECK(out == doctest::Approx(0.0));
    const double below = 700.0;
    phase_deg(s, 6, 0x60F, 4, &below, 1, kFs, &out);
    CHECK(std::fabs(out) > 1.0);

    const std::vector<double> grid = log_grid(10.0, 22800.0, 256);
    CHECK(composite_peak_db(s, 6, 0x60F, grid.data(), grid.size(), kFs) == doctest::Approx(3.0).epsilon(1e-6));
    // On its own layout nothing moves.
    magnitude_db(s, 8, 0x63F, 6, &f, 1, kFs, &out);
    CHECK(out == doctest::Approx(3.0).epsilon(1e-9));
}

TEST_CASE("the curve draws the values the processor plays") {
    // The processor clamps band gain to 60 dB, preamp and trims to -120..60 dB,
    // and widths to their range, and a level it has no value for stays at 0 dB.
    // The curve drew 80 dB for a gain of 80 and composite_peak_db returned 80
    // (review 2026-09-13).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EqState hostile;
    hostile.preamp_db = 100.0;
    hostile.channel_gain_db[0] = -500.0;
    hostile.channel_gain_db[1] = nan;
    hostile.channel_gain_db[2] = 70.0;
    EqState played;
    played.preamp_db = 60.0;
    played.channel_gain_db[0] = -120.0;
    played.channel_gain_db[1] = 0.0;
    played.channel_gain_db[2] = 60.0;

    const auto add = [&](Band h, double played_gain, double played_width) {
        hostile.bands.push_back(h);
        h.gain_db = played_gain;
        h.width = played_width;
        played.bands.push_back(h);
    };
    add(peaking(1000.0, 80.0, 1.0), 60.0, 1.0);
    add(peaking(300.0, -6.0, 1e-9), -6.0, 1e-4);
    Band lp;
    lp.type = FilterType::LowPass;
    lp.fc = 5000.0;
    lp.width = 1e6;
    add(lp, 0.0, 1000.0);
    Band shelf;
    shelf.type = FilterType::HighShelf;
    shelf.fc = 8000.0;
    shelf.gain_db = 6.0;
    shelf.width = 50.0;
    add(shelf, 6.0, 10.0);
    Band narrow = peaking(2000.0, 3.0, 1.0);
    narrow.width_mode = WidthMode::BandwidthOct;
    narrow.width = 1e-6;
    add(narrow, 3.0, 0.00145);
    // No value at all: the processor plays nothing for it, and the curve draws
    // nothing. At a frequency another band has, so the peak search points match.
    hostile.bands.push_back(peaking(1000.0, nan, 1.0));

    const std::vector<double> grid = log_grid(10.0, 22800.0, 256);
    std::vector<double> a(grid.size()), b(grid.size());
    for (uint32_t ch = 0; ch < 4; ++ch) {
        CAPTURE(ch);
        magnitude_db(hostile, 4, 0x33, ch, grid.data(), grid.size(), kFs, a.data());
        magnitude_db(played, 4, 0x33, ch, grid.data(), grid.size(), kFs, b.data());
        CHECK(a == b);
        phase_deg(hostile, 4, 0x33, ch, grid.data(), grid.size(), kFs, a.data());
        phase_deg(played, 4, 0x33, ch, grid.data(), grid.size(), kFs, b.data());
        CHECK(a == b);
    }
    for (size_t k = 0; k < played.bands.size(); ++k) {
        CAPTURE(k);
        band_magnitude_db(hostile.bands[k], grid.data(), grid.size(), kFs, a.data());
        band_magnitude_db(played.bands[k], grid.data(), grid.size(), kFs, b.data());
        CHECK(a == b);
    }
    CHECK(composite_peak_db(hostile, 4, 0x33, grid.data(), grid.size(), kFs) ==
          composite_peak_db(played, 4, 0x33, grid.data(), grid.size(), kFs));

    // A band with no frequency has no centre to look at: sampled at NaN, where
    // every response reads as unity, it put a 0 dB point under a cut.
    EqState cut;
    cut.bands.push_back(peaking(1000.0, -6.0, 0.3));
    EqState cut_and_nothing = cut;
    cut_and_nothing.bands.push_back(peaking(nan, 3.0, 1.0));
    const std::vector<double> mid = log_grid(300.0, 3000.0, 64);
    CHECK(composite_peak_db(cut, 2, 0x3, mid.data(), mid.size(), kFs) < -1.0);
    CHECK(composite_peak_db(cut_and_nothing, 2, 0x3, mid.data(), mid.size(), kFs) ==
          composite_peak_db(cut, 2, 0x3, mid.data(), mid.size(), kFs));

    EqState only_gain;
    only_gain.bands.push_back(peaking(1000.0, 80.0, 1.0));
    CHECK(mag_at(only_gain, 0, 1000.0) == doctest::Approx(60.0).epsilon(1e-9));
    CHECK(composite_peak_db(only_gain, 2, 0x3, grid.data(), grid.size(), kFs) == doctest::Approx(60.0).epsilon(1e-9));
}

TEST_CASE("composite_peak_db is the loudest sum of every band's dB on every channel") {
    // Designs each band once per call rather than once per frequency; the result
    // must be what designing them one frequency at a time gives.
    EqState s;
    for (uint32_t i = 0; i < 64; ++i) {
        Band b = peaking(20.0 * std::pow(1000.0, i / 63.0), (i % 2) != 0 ? 3.0 : -2.5, 0.5 + (i % 7),
                         (i % 3) == 0 ? kAllChannels : ChannelMask{1} << (i % 8));
        b.enabled = (i % 11) != 0;
        s.bands.push_back(b);
    }
    s.channel_gain_db[3] = 1.5;
    const std::vector<double> grid = log_grid(10.0, 22800.0, 512);
    double want = -1e9;
    for (uint32_t ch = 0; ch < 8; ++ch) {
        std::vector<double> points = grid;
        for (const Band& b : s.bands) {
            if (b.enabled && band_affects_channel(b, ch)) points.push_back(clamp_fc(b.fc, kFs));
        }
        for (double f : points) {
            double db = s.channel_gain_db[ch];
            for (const Band& b : s.bands) {
                if (b.enabled && band_affects_channel(b, ch)) db += magnitude_db(design(b, kFs), f, kFs);
            }
            want = std::max(want, db);
        }
    }
    CHECK(composite_peak_db(s, 8, 0x63F, grid.data(), grid.size(), kFs) == doctest::Approx(want).epsilon(1e-10));
}

TEST_CASE("phase of a cascade is the sum of the parts") {
    EqState s;
    s.bands.push_back(peaking(500.0, 6.0, 1.0));
    s.bands.push_back(peaking(4000.0, -6.0, 2.0));

    double freq = 1500.0;
    double composite = 0.0;
    phase_deg(s, 2, 0x3, 0, &freq, 1, kFs, &composite);

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
    magnitude_db(s, 2, 0x3, 0, grid.data(), grid.size(), kFs, out.data());

    for (double v : out) {
        CHECK(std::isfinite(v));
        CHECK(v > -40.0);
        CHECK(v < 20.0);
    }
    // Preamp of -6.1 dB against a composite peak: the result should not clip far
    // above 0 dB anywhere.
    const double peak = composite_peak_db(s, 2, 0x3, grid.data(), grid.size(), kFs);
    CHECK(peak + s.preamp_db < 1.0);
}
