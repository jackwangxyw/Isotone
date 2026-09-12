// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Cross-check against scipy. core/tests/reference/*.txt is produced by
// tools/gen_reference.py, which re-derives the same filters independently and
// evaluates them with scipy.signal.freqz. Agreement to 0.01 dB is the stage 2
// acceptance criterion.

#include "doctest.h"

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "isotone/biquad.h"

using namespace isotone;

namespace {

struct RefCase {
    std::string name;
    std::string type;
    double fc = 0.0;
    double gain_db = 0.0;
    double width = 0.0;
    std::string width_mode;
    bool shelf_corner = false;
    BiquadCoeffs coeffs;
    std::vector<double> magnitude_db;
    std::vector<double> phase_deg;
};

struct RefFile {
    double sample_rate = 0.0;
    std::vector<double> freqs;
    std::vector<RefCase> cases;
};

FilterType type_from_string(const std::string& s) {
    if (s == "peaking")    return FilterType::Peaking;
    if (s == "low_pass")   return FilterType::LowPass;
    if (s == "high_pass")  return FilterType::HighPass;
    if (s == "band_pass")  return FilterType::BandPass;
    if (s == "notch")      return FilterType::Notch;
    if (s == "all_pass")   return FilterType::AllPass;
    if (s == "low_shelf")  return FilterType::LowShelf;
    if (s == "high_shelf") return FilterType::HighShelf;
    FAIL("unknown filter type in reference data: " << s);
    return FilterType::Peaking;
}

WidthMode mode_from_string(const std::string& s) {
    if (s == "q")     return WidthMode::Q;
    if (s == "bw")    return WidthMode::BandwidthOct;
    if (s == "slope") return WidthMode::SlopeDb;
    FAIL("unknown width mode in reference data: " << s);
    return WidthMode::Q;
}

RefFile load(const std::string& path) {
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.good(), "cannot open reference file: " << path);

    RefFile ref;
    size_t points = 0, case_count = 0;
    std::string tok;

    // Skip the '#' comment lines, then read the keyed preamble.
    while (in >> tok) {
        if (tok.rfind("#", 0) == 0) {
            std::string rest;
            std::getline(in, rest);
            continue;
        }
        if (tok == "version") {
            int v = 0;
            in >> v;
            REQUIRE(v == 1);
        } else if (tok == "sample_rate") {
            in >> ref.sample_rate;
        } else if (tok == "points") {
            in >> points;
        } else if (tok == "cases") {
            in >> case_count;
        } else if (tok == "freqs") {
            ref.freqs.resize(points);
            for (size_t i = 0; i < points; ++i) in >> ref.freqs[i];
            break;
        }
    }

    REQUIRE(ref.sample_rate > 0.0);
    REQUIRE(points > 0);
    REQUIRE(ref.freqs.size() == points);

    while (in >> tok) {
        if (tok != "case") continue;
        RefCase c;
        int corner = 0;
        in >> c.name >> c.type >> c.fc >> c.gain_db >> c.width >> c.width_mode >> corner;
        c.shelf_corner = corner != 0;

        in >> tok;
        REQUIRE(tok == "coeffs");
        in >> c.coeffs.b0 >> c.coeffs.b1 >> c.coeffs.b2 >> c.coeffs.a1 >> c.coeffs.a2;

        in >> tok;
        REQUIRE(tok == "magnitude_db");
        c.magnitude_db.resize(points);
        for (size_t i = 0; i < points; ++i) in >> c.magnitude_db[i];

        in >> tok;
        REQUIRE(tok == "phase_deg");
        c.phase_deg.resize(points);
        for (size_t i = 0; i < points; ++i) in >> c.phase_deg[i];

        ref.cases.push_back(std::move(c));
    }

    REQUIRE(ref.cases.size() == case_count);
    return ref;
}

Band to_band(const RefCase& c) {
    Band b;
    b.type = type_from_string(c.type);
    b.fc = c.fc;
    b.gain_db = c.gain_db;
    b.width = c.width;
    b.width_mode = mode_from_string(c.width_mode);
    b.shelf_corner = c.shelf_corner;
    return b;
}

const char* kRates[] = {"44100", "48000", "96000", "192000"};

}  // namespace

TEST_CASE("core matches scipy reference magnitudes within 0.01 dB") {
    for (const char* rate : kRates) {
        const std::string path = std::string(ISOTONE_REFERENCE_DIR) + "/response_" + rate + ".txt";
        const RefFile ref = load(path);
        CAPTURE(rate);

        for (const RefCase& c : ref.cases) {
            CAPTURE(c.name);
            const BiquadCoeffs got = design(to_band(c), ref.sample_rate);

            // Coefficients first: a mismatch here localises the failure to the
            // design step rather than to the response evaluation.
            CHECK(got.b0 == doctest::Approx(c.coeffs.b0).epsilon(1e-12));
            CHECK(got.b1 == doctest::Approx(c.coeffs.b1).epsilon(1e-12));
            CHECK(got.b2 == doctest::Approx(c.coeffs.b2).epsilon(1e-12));
            CHECK(got.a1 == doctest::Approx(c.coeffs.a1).epsilon(1e-12));
            CHECK(got.a2 == doctest::Approx(c.coeffs.a2).epsilon(1e-12));

            double worst = 0.0;
            size_t worst_i = 0;
            for (size_t i = 0; i < ref.freqs.size(); ++i) {
                const double mine = magnitude_db(got, ref.freqs[i], ref.sample_rate);
                const double diff = std::abs(mine - c.magnitude_db[i]);
                if (diff > worst) {
                    worst = diff;
                    worst_i = i;
                }
            }
            CAPTURE(ref.freqs[worst_i]);
            CAPTURE(worst);
            CHECK(worst < 0.01);
        }
    }
}

TEST_CASE("core matches scipy reference phase within 0.01 degrees") {
    for (const char* rate : kRates) {
        const std::string path = std::string(ISOTONE_REFERENCE_DIR) + "/response_" + rate + ".txt";
        const RefFile ref = load(path);
        CAPTURE(rate);

        for (const RefCase& c : ref.cases) {
            CAPTURE(c.name);
            const BiquadCoeffs got = design(to_band(c), ref.sample_rate);
            double worst = 0.0;
            for (size_t i = 0; i < ref.freqs.size(); ++i) {
                double diff = std::abs(phase_deg(got, ref.freqs[i], ref.sample_rate) -
                                       c.phase_deg[i]);
                // Both are wrapped to (-180, 180]; treat the seam as zero error.
                if (diff > 180.0) diff = 360.0 - diff;
                worst = std::max(worst, diff);
            }
            CAPTURE(worst);
            CHECK(worst < 0.01);
        }
    }
}

TEST_CASE("reference files cover every filter type") {
    const RefFile ref = load(std::string(ISOTONE_REFERENCE_DIR) + "/response_48000.txt");
    bool seen[8] = {false, false, false, false, false, false, false, false};
    for (const RefCase& c : ref.cases) {
        seen[static_cast<size_t>(type_from_string(c.type))] = true;
    }
    for (size_t i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(seen[i]);
    }
}
