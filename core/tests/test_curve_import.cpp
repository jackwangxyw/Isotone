// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Importing a magnitude curve: the GraphicEQ and FilterCurve lines, and the
// bands fitted to them. The owner exported a FilterCurve from Peace and it
// imported as nothing at all (2026-09-15).

#include "doctest.h"

#include <cmath>
#include <string>
#include <vector>

#include "isotone/curve_import.h"
#include "isotone/response.h"

using namespace isotone;

namespace {

constexpr double kRate = 48000.0;

// The composite of a fit at `hz`.
double fitted_db(const CurveFit& fit, double hz) {
    EqState state;
    state.bands = fit.bands;
    double out = 0;
    magnitude_db(state, 2, 0, 0, &hz, 1, kRate, &out);
    return out;
}

// A curve sampled from a state, as an export of it would hold.
std::vector<CurvePoint> curve_of(const EqState& state, size_t points) {
    const std::vector<double> freqs = log_grid(20.0, 20000.0, points);
    std::vector<double> db(points);
    magnitude_db(state, 2, 0, 0, freqs.data(), points, kRate, db.data());
    std::vector<CurvePoint> curve(points);
    for (size_t i = 0; i < points; ++i) curve[i] = {freqs[i], db[i]};
    return curve;
}

Band peaking(uint32_t id, double fc, double gain, double q) {
    Band b;
    b.id = id;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain;
    b.width = q;
    return b;
}

}  // namespace

TEST_CASE("a GraphicEQ line reads as its points") {
    const std::vector<CurvePoint> curve = parse_curve("GraphicEQ: 20 -1.2; 100 3; 1000 0; 10000 -6.5");
    REQUIRE(curve.size() == 4);
    CHECK(curve[0].hz == 20.0);
    CHECK(curve[0].db == doctest::Approx(-1.2));
    CHECK(curve[3].hz == 10000.0);
    CHECK(curve[3].db == doctest::Approx(-6.5));

    // In a file with other lines, and out of order.
    const std::vector<CurvePoint> inside =
        parse_curve("Device: Headphones\nChannel: L R\nGraphicEQ: 1000 2; 100 -3\n");
    REQUIRE(inside.size() == 2);
    CHECK(inside[0].hz == 100.0);
    CHECK(inside[1].hz == 1000.0);

    CHECK(parse_curve("Filter: ON PK Fc 1000 Hz Gain -3 dB Q 1.41").empty());
    CHECK(parse_curve("").empty());
}

TEST_CASE("a FilterCurve line reads as its points") {
    // The shape of the owner's export, cut to four points.
    const std::vector<CurvePoint> curve = parse_curve(
        "FilterCurve:f0=\"10\" f1=\"100.7\" f2=\"1014\" f3=\"10211\" FilterLength=\"8191\" InterpolateLin=\"0\" "
        "InterpolationMethod=\"B-spline\" v0=\"-39.956\" v1=\"0.667\" v2=\"-1.231\" v3=\"4.929\"");
    REQUIRE(curve.size() == 4);
    CHECK(curve[0].hz == doctest::Approx(10.0));
    CHECK(curve[0].db == doctest::Approx(-39.956));
    CHECK(curve[1].hz == doctest::Approx(100.7));
    CHECK(curve[1].db == doctest::Approx(0.667));
    CHECK(curve[3].hz == doctest::Approx(10211.0));
    CHECK(curve[3].db == doctest::Approx(4.929));

    // A value without its frequency, or the other way round, is left out.
    CHECK(parse_curve("FilterCurve:f0=\"100\" f1=\"200\" v0=\"3\"").size() == 1);
}

TEST_CASE("the fit follows a curve made of peaking filters") {
    EqState source;
    source.bands = {peaking(1, 80, 6, 0.9), peaking(2, 400, -4.5, 1.6), peaking(3, 3000, 5, 2.2),
                    peaking(4, 9000, -3, 1.2)};
    const CurveFit fit = fit_curve(curve_of(source, 128), kRate);
    REQUIRE(!fit.bands.empty());
    MESSAGE("bands " << fit.bands.size() << " worst " << fit.worst_db << " rms " << fit.rms_db);
    CHECK(fit.rms_db < 0.5);
    CHECK(fit.worst_db < 1.5);
    // Four filters made the curve, and about four come back.
    CHECK(fit.bands.size() <= 6);
    for (double hz : {40.0, 80.0, 200.0, 400.0, 1000.0, 3000.0, 9000.0, 15000.0}) {
        CAPTURE(hz);
        double want = 0;
        magnitude_db(source, 2, 0, 0, &hz, 1, kRate, &want);
        CHECK(std::abs(fitted_db(fit, hz) - want) < 1.5);
    }
}

TEST_CASE("the fit follows the owner's exported curve") {
    // TC8FD05-04 EQ.txt, every point of it: a deep bass cut rising to flat, a dip
    // around 250 Hz and a lift at 10 kHz.
    const std::string line =
        "FilterCurve:f0=\"10\" f1=\"11.7\" f2=\"13.6\" f3=\"15.9\" f4=\"18.5\" f5=\"21.6\" f6=\"25.2\" f7=\"29.4\" "
        "f8=\"34.3\" f9=\"40\" f10=\"46.6\" f11=\"54.4\" f12=\"63.4\" f13=\"74\" f14=\"86.3\" f15=\"100.7\" "
        "f16=\"117.5\" f17=\"137\" f18=\"159.8\" f19=\"186.4\" f20=\"217.5\" f21=\"253.6\" f22=\"295.9\" "
        "f23=\"345.1\" f24=\"402.6\" f25=\"469.6\" f26=\"547.7\" f27=\"638.9\" f28=\"745.3\" f29=\"869.3\" "
        "f30=\"1014\" f31=\"1182.8\" f32=\"1379.7\" f33=\"1609.3\" f34=\"1877.2\" f35=\"2189.7\" f36=\"2554.2\" "
        "f37=\"2979.4\" f38=\"3475.3\" f39=\"4053.8\" f40=\"4728.5\" f41=\"5515.6\" f42=\"6433.7\" f43=\"7504.7\" "
        "f44=\"8753.9\" f45=\"10211\" f46=\"11910.7\" f47=\"13893.3\" f48=\"16205.9\" f49=\"18903.4\" "
        "FilterLength=\"8191\" InterpolateLin=\"0\" InterpolationMethod=\"B-spline\" "
        "v0=\"-39.956\" v1=\"-37.212\" v2=\"-34.576\" v3=\"-31.832\" v4=\"-29.162\" v5=\"-26.417\" v6=\"-23.667\" "
        "v7=\"-20.894\" v8=\"-18.09\" v9=\"-15.258\" v10=\"-12.407\" v11=\"-9.486\" v12=\"-6.601\" v13=\"-3.793\" "
        "v14=\"-1.297\" v15=\"0.667\" v16=\"1.981\" v17=\"2.558\" v18=\"1.862\" v19=\"-0.004\" v20=\"-1.741\" "
        "v21=\"-2.299\" v22=\"-1.887\" v23=\"-1.313\" v24=\"-0.899\" v25=\"-0.662\" v26=\"-0.564\" v27=\"-0.581\" "
        "v28=\"-0.705\" v29=\"-0.93\" v30=\"-1.231\" v31=\"-1.55\" v32=\"-1.807\" v33=\"-1.932\" v34=\"-1.883\" "
        "v35=\"-1.64\" v36=\"-1.215\" v37=\"-0.652\" v38=\"-0.031\" v39=\"0.572\" v40=\"1.122\" v41=\"1.638\" "
        "v42=\"2.171\" v43=\"2.781\" v44=\"3.512\" v45=\"4.323\" v46=\"4.929\" v47=\"4.726\" v48=\"3.537\" "
        "v49=\"2.101\"";
    const std::vector<CurvePoint> curve = parse_curve(line);
    REQUIRE(curve.size() == 50);

    const CurveFit fit = fit_curve(curve, kRate);
    MESSAGE("bands " << fit.bands.size() << " worst " << fit.worst_db << " rms " << fit.rms_db);
    // The owner: "it uses a high shelf along with a high pass filter, thats why"
    // the bottom would not fit peaking bands. The fit picks up both, and then the
    // whole curve follows, 10 Hz included (worst 0.46 dB, measured; it was 3.9 dB
    // at 22 Hz with bands alone).
    bool has_high_pass = false, has_high_shelf = false;
    for (const Band& b : fit.bands) {
        has_high_pass = has_high_pass || b.type == FilterType::HighPass;
        has_high_shelf = has_high_shelf || b.type == FilterType::HighShelf;
    }
    CHECK(has_high_pass);
    CHECK(has_high_shelf);
    // 0.21 dB rms over the whole curve, 10 Hz to 18.9 kHz. What is left sits in the
    // top octave, where the curve turns over faster than a band near Nyquist can.
    CHECK(fit.worst_db < 2.0);
    const auto target_at = [&](double hz) {
        for (size_t i = 1; i < curve.size(); ++i) {
            if (curve[i].hz < hz) continue;
            const double t = std::log(hz / curve[i - 1].hz) / std::log(curve[i].hz / curve[i - 1].hz);
            return curve[i - 1].db + (curve[i].db - curve[i - 1].db) * t;
        }
        return curve.back().db;
    };
    for (double hz = 100.0; hz <= 10000.0; hz *= 1.05) {
        CAPTURE(hz);
        CHECK(std::abs(fitted_db(fit, hz) - target_at(hz)) < 1.0);
    }
    for (double hz = 10.0; hz < 100.0; hz *= 1.05) {
        CAPTURE(hz);
        CHECK(std::abs(fitted_db(fit, hz) - target_at(hz)) < 1.0);
    }
    REQUIRE(!fit.bands.empty());
    // As few filters as follow it: the owner made this in Peace with thirteen
    // sliders, six of them at 0 dB, and a fit of one band per third of an octave
    // gave thirty filters back (2026-09-15). Ten, and 0.21 dB rms.
    CHECK(fit.bands.size() <= 12);
    CHECK(fit.rms_db < 0.3);
    // The shape the owner knows: down at the bottom, up around 130 Hz, a dip near
    // 250 Hz, and a lift at 10 kHz.
    // At the file's own points, not between them.
    // Absolute dB, at the file's own points rather than between them.
    CHECK(std::abs(fitted_db(fit, 10.0) - -39.956) < 1.0);
    CHECK(std::abs(fitted_db(fit, 21.6) - -26.417) < 0.6);
    CHECK(fitted_db(fit, 40.0) == doctest::Approx(-15.26).epsilon(0.15));
    CHECK(std::abs(fitted_db(fit, 137.0) - 2.558) < 0.5);
    CHECK(std::abs(fitted_db(fit, 253.6) - -2.299) < 0.5);
    CHECK(std::abs(fitted_db(fit, 10211.0) - 4.323) < 0.5);
    CHECK(std::abs(fitted_db(fit, 18903.4) - 2.101) < 2.0);
}

TEST_CASE("a flat curve fits no bands, and a short one none at all") {
    std::vector<CurvePoint> flat;
    for (double hz = 20; hz <= 20000; hz *= 1.5) flat.push_back({hz, 0.0});
    const CurveFit none = fit_curve(flat, kRate);
    CHECK(none.bands.empty());
    CHECK(none.worst_db < 0.05);

    CHECK(fit_curve({}, kRate).bands.empty());
    CHECK(fit_curve({{1000.0, 3.0}}, kRate).bands.empty());
}

TEST_CASE("the fit's gains are held to the limit it is given") {
    std::vector<CurvePoint> steep;
    for (double hz = 20; hz <= 20000; hz *= 1.2) steep.push_back({hz, hz < 200 ? -40.0 : 30.0});
    const CurveFit fit = fit_curve(steep, kRate, 12.0);
    for (const Band& b : fit.bands) {
        CAPTURE(b.fc);
        CHECK(std::abs(b.gain_db) <= 12.0 + 1e-9);
    }
}

TEST_CASE("a curve made with a high pass and a high shelf is fitted with them") {
    // What the owner's export was made with. Bands alone cannot hold a 12 dB per
    // octave rolloff, so the fit looks for these shapes first.
    EqState source;
    Band hp;
    hp.id = 1;
    hp.type = FilterType::HighPass;
    hp.fc = 45;
    hp.width = 0.707;
    Band shelf;
    shelf.id = 2;
    shelf.type = FilterType::HighShelf;
    shelf.fc = 4000;
    shelf.gain_db = 5;
    shelf.width = 0.707;
    source.bands = {hp, shelf, peaking(3, 300, -3, 1.4)};

    const CurveFit fit = fit_curve(curve_of(source, 128), kRate);
    MESSAGE("bands " << fit.bands.size() << " worst " << fit.worst_db << " rms " << fit.rms_db);
    REQUIRE(!fit.bands.empty());
    CHECK(fit.bands[0].type == FilterType::HighPass);
    CHECK(fit.bands[0].fc == doctest::Approx(45).epsilon(0.25));
    CHECK(fit.rms_db < 0.4);
    CHECK(fit.bands.size() <= 4);   // the three it was made of, give or take one
    for (double hz : {20.0, 30.0, 45.0, 100.0, 300.0, 1000.0, 8000.0}) {
        CAPTURE(hz);
        double want = 0;
        magnitude_db(source, 2, 0, 0, &hz, 1, kRate, &want);
        CHECK(std::abs(fitted_db(fit, hz) - want) < 1.0);
    }
}
