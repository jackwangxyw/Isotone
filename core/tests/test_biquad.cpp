// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Properties of a single biquad that must hold regardless of the reference data:
// gain at the centre frequency, asymptotic behaviour far from it, stability, and
// graceful handling of nonsense input.

#include "doctest.h"

#include <cmath>

#include "isotone/biquad.h"

using namespace isotone;

namespace {

Band peaking(double fc, double gain_db, double q) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    b.width_mode = WidthMode::Q;
    return b;
}

double mag_at(const Band& b, double freq, double fs) {
    return magnitude_db(design(b, fs), freq, fs);
}

constexpr double kFs = 48000.0;

}  // namespace

TEST_CASE("peaking filter hits its gain exactly at fc") {
    for (double gain : {-18.0, -12.0, -6.0, -0.5, 0.5, 6.0, 12.0, 18.0}) {
        for (double q : {0.3, 0.7071, 1.0, 4.0, 12.0}) {
            for (double fc : {30.0, 100.0, 1000.0, 8000.0, 16000.0}) {
                const double got = mag_at(peaking(fc, gain, q), fc, kFs);
                CAPTURE(gain);
                CAPTURE(q);
                CAPTURE(fc);
                CHECK(got == doctest::Approx(gain).epsilon(0.0).scale(1.0).epsilon(1e-9));
            }
        }
    }
}

TEST_CASE("peaking filter is flat far from fc") {
    const Band b = peaking(1000.0, 12.0, 4.0);
    CHECK(mag_at(b, 20.0, kFs) == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(mag_at(b, 20000.0, kFs) == doctest::Approx(0.0).epsilon(1e-3));
}

TEST_CASE("zero gain peaking is the identity") {
    const BiquadCoeffs c = design(peaking(1000.0, 0.0, 1.0), kFs);
    for (double f : {20.0, 200.0, 2000.0, 20000.0}) {
        CHECK(magnitude_db(c, f, kFs) == doctest::Approx(0.0).epsilon(1e-12));
    }
}

TEST_CASE("peaking gain is symmetric in sign") {
    // A +6 dB bell and a -6 dB bell of the same Q are mirror images. This is a
    // property of the RBJ peaking form, and it catches an A vs 1/A swap.
    const double up   = mag_at(peaking(1000.0, 6.0, 2.0), 1200.0, kFs);
    const double down = mag_at(peaking(1000.0, -6.0, 2.0), 1200.0, kFs);
    CHECK(up == doctest::Approx(-down).epsilon(1e-9));
}

TEST_CASE("low pass and high pass are -3 dB at fc with Q = 1/sqrt(2)") {
    const double q = 0.70710678118654752;
    Band lp;
    lp.type = FilterType::LowPass;
    lp.fc = 1000.0;
    lp.width = q;
    Band hp = lp;
    hp.type = FilterType::HighPass;

    CHECK(mag_at(lp, 1000.0, kFs) == doctest::Approx(-3.0103).epsilon(1e-3));
    CHECK(mag_at(hp, 1000.0, kFs) == doctest::Approx(-3.0103).epsilon(1e-3));

    // Second order means about 12 dB per octave in the stopband. The figure is
    // never exactly 12: a couple of octaves above fc the asymptote is not yet
    // reached, and near nyquist the bilinear transform warps the axis. Measured
    // at 192 kHz over 4-8 kHz, where both effects are small, it is 12.10 dB.
    const double slope = mag_at(lp, 4000.0, 192000.0) - mag_at(lp, 8000.0, 192000.0);
    CHECK(slope == doctest::Approx(12.10).epsilon(0.01));
}

TEST_CASE("low pass rolloff steepens toward nyquist") {
    // The same filter at 48 kHz: 13.3 dB over 4-8 kHz and 19.1 dB over 8-16 kHz,
    // because a digital biquad's response is squeezed as it approaches nyquist.
    // Asserted so the warping is pinned down rather than mistaken for a bug.
    Band lp;
    lp.type = FilterType::LowPass;
    lp.fc = 1000.0;
    lp.width = 0.70710678118654752;
    const double near_fc  = mag_at(lp, 2000.0, kFs) - mag_at(lp, 4000.0, kFs);
    const double mid      = mag_at(lp, 4000.0, kFs) - mag_at(lp, 8000.0, kFs);
    const double near_nyq = mag_at(lp, 8000.0, kFs) - mag_at(lp, 16000.0, kFs);
    CHECK(near_fc  == doctest::Approx(12.10).epsilon(0.01));
    CHECK(mid      == doctest::Approx(13.32).epsilon(0.01));
    CHECK(near_nyq == doctest::Approx(19.08).epsilon(0.01));
    CHECK(near_fc < mid);
    CHECK(mid < near_nyq);
}

TEST_CASE("low pass passes DC and high pass blocks it") {
    Band lp;
    lp.type = FilterType::LowPass;
    lp.fc = 1000.0;
    lp.width = 0.7071;
    Band hp = lp;
    hp.type = FilterType::HighPass;

    CHECK(mag_at(lp, 10.0, kFs) == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(mag_at(hp, 10.0, kFs) < -35.0);
}

TEST_CASE("notch reaches deep attenuation at fc and passes either side") {
    Band n;
    n.type = FilterType::Notch;
    n.fc = 1000.0;
    n.width = 30.0;
    CHECK(mag_at(n, 1000.0, kFs) < -60.0);
    CHECK(mag_at(n, 500.0, kFs) == doctest::Approx(0.0).epsilon(1e-2));
    CHECK(mag_at(n, 2000.0, kFs) == doctest::Approx(0.0).epsilon(1e-2));
}

TEST_CASE("all pass is unity magnitude everywhere but shifts phase") {
    Band ap;
    ap.type = FilterType::AllPass;
    ap.fc = 1000.0;
    ap.width = 1.0;
    const BiquadCoeffs c = design(ap, kFs);
    for (double f : {20.0, 100.0, 1000.0, 5000.0, 20000.0}) {
        CAPTURE(f);
        CHECK(magnitude_db(c, f, kFs) == doctest::Approx(0.0).epsilon(1e-9));
    }
    // 180 degrees of shift at fc, by construction.
    CHECK(std::abs(phase_deg(c, 1000.0, kFs)) == doctest::Approx(180.0).epsilon(1e-6));
}

TEST_CASE("band pass peaks at 0 dB regardless of Q, and rolls off both ways") {
    // Constant 0 dB peak gain variant (matching upstream Equalizer APO): the top
    // of the passband is unity for every Q; Q only sets the width.
    for (double q : {0.5, 1.0, 2.0, 8.0}) {
        Band bp;
        bp.type = FilterType::BandPass;
        bp.fc = 1000.0;
        bp.width = q;
        CAPTURE(q);
        CHECK(mag_at(bp, 1000.0, kFs) == doctest::Approx(0.0).epsilon(1e-9));
    }

    Band bp;
    bp.type = FilterType::BandPass;
    bp.fc = 1000.0;
    bp.width = 2.0;
    CHECK(mag_at(bp, 100.0, kFs) < -20.0);
    CHECK(mag_at(bp, 10000.0, kFs) < -20.0);

    // Q is defined as fc divided by the -3 dB bandwidth, so Q = 2 at 1 kHz means
    // a 500 Hz wide band. Its edges are not fc +/- 250 Hz but geometrically
    // placed around fc: f0 * (sqrt(1 + 1/(4Q^2)) +/- 1/(2Q)), which is 780.8 and
    // 1280.8 Hz. Their product is fc^2 and their difference is fc/Q.
    const double q = 2.0, f0 = 1000.0;
    const double centre = std::sqrt(1.0 + 1.0 / (4.0 * q * q));
    const double f_lo = f0 * (centre - 1.0 / (2.0 * q));
    const double f_hi = f0 * (centre + 1.0 / (2.0 * q));
    CHECK(f_hi - f_lo == doctest::Approx(f0 / q).epsilon(1e-12));
    CHECK(f_lo * f_hi == doctest::Approx(f0 * f0).epsilon(1e-12));
    CHECK(mag_at(bp, f_lo, kFs) == doctest::Approx(-3.0).epsilon(0.02));
    CHECK(mag_at(bp, f_hi, kFs) == doctest::Approx(-3.0).epsilon(0.02));
}

TEST_CASE("shelves reach their full gain in the passband and unity in the stopband") {
    Band ls;
    ls.type = FilterType::LowShelf;
    ls.fc = 200.0;
    ls.gain_db = 6.0;
    ls.width = 0.7071;
    ls.width_mode = WidthMode::Q;

    CHECK(mag_at(ls, 10.0, kFs) == doctest::Approx(6.0).epsilon(1e-3));
    CHECK(mag_at(ls, 20000.0, kFs) == doctest::Approx(0.0).epsilon(1e-3));
    // Midpoint of a shelf sits at half the gain in dB.
    CHECK(mag_at(ls, 200.0, kFs) == doctest::Approx(3.0).epsilon(1e-6));

    Band hs = ls;
    hs.type = FilterType::HighShelf;
    hs.fc = 5000.0;
    CHECK(mag_at(hs, 20000.0, kFs) == doctest::Approx(6.0).epsilon(1e-2));
    CHECK(mag_at(hs, 20.0, kFs) == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(mag_at(hs, 5000.0, kFs) == doctest::Approx(3.0).epsilon(1e-6));
}

TEST_CASE("shelf_corner shifts the design frequency the way Equalizer APO does") {
    Band plain;
    plain.type = FilterType::LowShelf;
    plain.fc = 1000.0;
    plain.gain_db = 6.0;
    plain.width = 0.9;
    plain.width_mode = WidthMode::SlopeDb;
    plain.shelf_corner = false;

    Band corner = plain;
    corner.shelf_corner = true;

    // Both tend to the same asymptote far below the knee. Slope 0.9 dB is an
    // extremely gentle shelf, so at 10 Hz it is still a hundredth of a dB short
    // of its full 6 dB; that is the filter, not an error.
    CHECK(mag_at(plain, 10.0, kFs) == doctest::Approx(6.0).epsilon(2e-3));
    CHECK(mag_at(corner, 10.0, kFs) == doctest::Approx(6.0).epsilon(2e-3));

    // The knee is what moves. A low shelf with the corner correction is shifted
    // up in frequency, so at fc it has already delivered more of its gain.
    CHECK(mag_at(corner, 1000.0, kFs) > mag_at(plain, 1000.0, kFs) + 0.05);
    // Plain LSC is by definition at exactly half its gain at fc.
    CHECK(mag_at(plain, 1000.0, kFs) == doctest::Approx(3.0).epsilon(1e-6));
}

TEST_CASE("designed filters are stable across an aggressive parameter sweep") {
    const FilterType types[] = {
        FilterType::Peaking,  FilterType::LowPass,   FilterType::HighPass,
        FilterType::BandPass, FilterType::Notch,     FilterType::AllPass,
        FilterType::LowShelf, FilterType::HighShelf,
    };
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (FilterType t : types) {
            for (double fc : {1.0, 10.0, 20.0, 1000.0, 19000.0, 23000.0, 1e6}) {
                for (double q : {0.05, 0.5, 1.0, 20.0, 100.0}) {
                    for (double g : {-40.0, -6.0, 0.0, 6.0, 40.0}) {
                        Band b;
                        b.type = t;
                        b.fc = fc;
                        b.gain_db = g;
                        b.width = q;
                        CAPTURE(fs);
                        CAPTURE(static_cast<int>(t));
                        CAPTURE(fc);
                        CAPTURE(q);
                        CAPTURE(g);
                        const BiquadCoeffs c = design(b, fs);
                        CHECK(is_stable(c));
                        CHECK(std::isfinite(c.b0));
                        CHECK(std::isfinite(c.b1));
                        CHECK(std::isfinite(c.b2));
                        CHECK(std::isfinite(c.a1));
                        CHECK(std::isfinite(c.a2));
                    }
                }
            }
        }
    }
}

TEST_CASE("invalid input degrades to the identity rather than producing NaN") {
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();

    Band b = peaking(1000.0, 6.0, 1.0);
    CHECK(design(b, 0.0).b0 == 1.0);
    CHECK(design(b, -48000.0).b0 == 1.0);

    b.enabled = false;
    CHECK(design(b, kFs).b0 == 1.0);

    b.enabled = true;
    b.width = 0.0;
    CHECK(design(b, kFs).b0 == 1.0);

    b = peaking(nan, 6.0, 1.0);
    CHECK(design(b, kFs).b0 == 1.0);

    b = peaking(1000.0, inf, 1.0);
    CHECK(design(b, kFs).b0 == 1.0);
}

TEST_CASE("fc is clamped into the representable range") {
    CHECK(clamp_fc(1.0, 48000.0) == doctest::Approx(10.0));
    CHECK(clamp_fc(1e9, 48000.0) == doctest::Approx(48000.0 * 0.5 * 0.95));
    CHECK(clamp_fc(1000.0, 48000.0) == doctest::Approx(1000.0));
}

TEST_CASE("response outside the valid band is unity") {
    const BiquadCoeffs c = design(peaking(1000.0, 12.0, 1.0), kFs);
    CHECK(std::abs(response(c, -5.0, kFs)) == doctest::Approx(1.0));
    CHECK(std::abs(response(c, 0.0, kFs)) == doctest::Approx(1.0));
    CHECK(std::abs(response(c, 30000.0, kFs)) == doctest::Approx(1.0));
}
