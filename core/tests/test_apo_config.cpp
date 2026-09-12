// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Equalizer APO config format. The fixtures here are shaped like the files that
// actually exist in the wild: AutoEq exports, Room EQ Wizard exports, and what
// Peace writes.

#include "doctest.h"

#include <cmath>
#include <string>

#include "isotone/apo_config.h"
#include "isotone/response.h"

using namespace isotone;

namespace {

constexpr double kFs = 48000.0;

const char* const kAutoEq =
    "Preamp: -6.1 dB\n"
    "Filter 1: ON LSC Fc 105 Hz Gain 6.4 dB Q 0.70\n"
    "Filter 2: ON PK Fc 8800 Hz Gain 5.1 dB Q 1.42\n"
    "Filter 3: ON PK Fc 118 Hz Gain -3.1 dB Q 0.50\n"
    "Filter 4: ON PK Fc 2100 Hz Gain -2.4 dB Q 2.00\n"
    "Filter 5: ON PK Fc 5300 Hz Gain 1.9 dB Q 3.10\n"
    "Filter 6: ON HSC Fc 10000 Hz Gain -2.1 dB Q 0.70\n";

// Peace numbers its filter slots sparsely, writes HS with no Q at all, and
// scopes with Device/Channel lines.
const char* const kPeaceShaped =
    "Device: all\n"
    "Channel: all\n"
    "Preamp: -5 dB\n"
    "Filter 3: ON HPQ Fc 100 Hz Q 1\n"
    "Filter 4: ON PK Fc 150 Hz Gain 3 dB Q 1.4\n"
    "Filter 8: ON HS Fc 1100 Hz Gain -3 dB\n"
    "Filter 12: ON PK Fc 12500 Hz Gain 4 dB Q 0.7\n";

}  // namespace

TEST_CASE("an AutoEq export round-trips") {
    const ApoParseResult r = parse_apo_config(kAutoEq);
    REQUIRE(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 6);
    CHECK(r.state.preamp_db == doctest::Approx(-6.1));

    CHECK(r.state.bands[0].type == FilterType::LowShelf);
    CHECK(r.state.bands[0].fc == doctest::Approx(105.0));
    CHECK(r.state.bands[0].gain_db == doctest::Approx(6.4));
    CHECK(r.state.bands[0].width == doctest::Approx(0.70));
    CHECK(r.state.bands[0].width_mode == WidthMode::Q);
    CHECK_FALSE(r.state.bands[0].shelf_corner);   // LSC, so no corner correction

    CHECK(r.state.bands[5].type == FilterType::HighShelf);
    CHECK_FALSE(r.state.bands[5].shelf_corner);

    // Re-export, re-import, and compare the curves rather than the text.
    const std::string text = format_apo_config(r.state);
    const ApoParseResult again = parse_apo_config(text);
    REQUIRE(again.warnings.empty());
    REQUIRE(again.state.bands.size() == r.state.bands.size());

    const std::vector<double> grid = log_grid(20.0, 20000.0, 256);
    std::vector<double> a(grid.size()), b(grid.size());
    magnitude_db(r.state, 0, grid.data(), grid.size(), kFs, a.data());
    magnitude_db(again.state, 0, grid.data(), grid.size(), kFs, b.data());
    for (size_t i = 0; i < grid.size(); ++i) {
        CAPTURE(grid[i]);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-9));
    }
}

TEST_CASE("a Peace-shaped config parses despite sparse filter numbers") {
    const ApoParseResult r = parse_apo_config(kPeaceShaped);
    REQUIRE(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 4);
    CHECK(r.state.preamp_db == doctest::Approx(-5.0));
    REQUIRE(r.devices.size() == 1);
    CHECK(r.devices[0] == "all");

    CHECK(r.state.bands[0].type == FilterType::HighPass);
    CHECK(r.state.bands[0].width == doctest::Approx(1.0));

    // HS with no width at all: upstream sets S = 0.9 directly and skips both the
    // divide-by-12 and the corner correction, because those live in a branch
    // that only runs when a width was given. Stored as a 10.8 dB slope, since
    // design() divides by 12.
    CHECK(r.state.bands[2].type == FilterType::HighShelf);
    CHECK(r.state.bands[2].width_mode == WidthMode::SlopeDb);
    CHECK(r.state.bands[2].width == doctest::Approx(10.8));
    CHECK_FALSE(r.state.bands[2].shelf_corner);
}

TEST_CASE("the corner correction needs both a bare token and an explicit width") {
    // This is the distinction that a measurement through a real Equalizer APO
    // caught, after the first reading of upstream got it wrong. Only the first
    // of these four gets the design-frequency shift.
    struct Case { const char* text; bool corner; };
    const Case cases[] = {
        {"Filter 1: ON LS Fc 1000 Hz Gain 6 dB Q 0.7\n",  true},   // bare token, explicit Q
        {"Filter 1: ON LSC Fc 1000 Hz Gain 6 dB Q 0.7\n", false},  // C token
        {"Filter 1: ON LS Fc 1000 Hz Gain 6 dB\n",        false},  // bare token, no width
        {"Filter 1: ON LSC Fc 1000 Hz Gain 6 dB\n",       false},  // C token, no width
    };
    for (const Case& c : cases) {
        const ApoParseResult r = parse_apo_config(c.text);
        CAPTURE(c.text);
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].shelf_corner == c.corner);
    }

    // The two no-width cases must be identical filters, not merely both
    // uncorrected.
    const ApoParseResult a = parse_apo_config("Filter 1: ON LS Fc 1000 Hz Gain 6 dB\n");
    const ApoParseResult b = parse_apo_config("Filter 1: ON LSC Fc 1000 Hz Gain 6 dB\n");
    double f = 1000.0, ma = 0.0, mb = 0.0;
    band_magnitude_db(a.state.bands[0], &f, 1, kFs, &ma);
    band_magnitude_db(b.state.bands[0], &f, 1, kFs, &mb);
    CHECK(ma == doctest::Approx(mb).epsilon(1e-12));
    // S = 0.9 is a steep shelf: half the gain at fc and a narrow transition. The
    // mistaken S = 0.075 was far gentler and read well below 6 dB down at 250 Hz.
    CHECK(ma == doctest::Approx(3.0).epsilon(1e-6));
    double f2 = 250.0, m2 = 0.0;
    band_magnitude_db(a.state.bands[0], &f2, 1, kFs, &m2);
    CHECK(m2 > 5.5);
}

TEST_CASE("LS and LSC design differently, and the token survives a round trip") {
    const ApoParseResult with    = parse_apo_config("Filter 1: ON LS Fc 1000 Hz Gain 6 dB Q 0.7\n");
    const ApoParseResult without = parse_apo_config("Filter 1: ON LSC Fc 1000 Hz Gain 6 dB Q 0.7\n");
    REQUIRE(with.state.bands.size() == 1);
    REQUIRE(without.state.bands.size() == 1);
    CHECK(with.state.bands[0].shelf_corner);
    CHECK_FALSE(without.state.bands[0].shelf_corner);

    double f = 1000.0, a = 0.0, b = 0.0;
    band_magnitude_db(with.state.bands[0], &f, 1, kFs, &a);
    band_magnitude_db(without.state.bands[0], &f, 1, kFs, &b);
    CHECK(a != doctest::Approx(b));

    CHECK(format_apo_config(with.state).find(" LS ") != std::string::npos);
    CHECK(format_apo_config(without.state).find(" LSC ") != std::string::npos);
}

TEST_CASE("PEQ and Modal are accepted as aliases for PK") {
    for (const char* token : {"PK", "PEQ", "Modal"}) {
        const std::string text =
            std::string("Filter 1: ON ") + token + " Fc 1000 Hz Gain 3 dB Q 1\n";
        const ApoParseResult r = parse_apo_config(text);
        CAPTURE(token);
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].type == FilterType::Peaking);
        CHECK(r.state.bands[0].gain_db == doctest::Approx(3.0));
    }
}

TEST_CASE("bandwidth in octaves is read as its own width mode") {
    const ApoParseResult r = parse_apo_config("Filter 1: ON PK Fc 1000 Hz Gain 6 dB BW Oct 0.5\n");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].width_mode == WidthMode::BandwidthOct);
    CHECK(r.state.bands[0].width == doctest::Approx(0.5));
    CHECK(format_apo_config(r.state).find("BW Oct 0.5") != std::string::npos);
}

TEST_CASE("a shelf slope stated in dB is read from in front of Fc") {
    const ApoParseResult r = parse_apo_config("Filter 1: ON LS 6 dB Fc 100 Hz Gain 5 dB\n");
    REQUIRE(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].type == FilterType::LowShelf);
    CHECK(r.state.bands[0].width_mode == WidthMode::SlopeDb);
    CHECK(r.state.bands[0].width == doctest::Approx(6.0));
    CHECK(r.state.bands[0].fc == doctest::Approx(100.0));
    CHECK(r.state.bands[0].gain_db == doctest::Approx(5.0));
}

TEST_CASE("upstream's width defaults are applied when the field is missing") {
    struct Case { const char* text; FilterType type; double width; WidthMode mode; };
    const Case cases[] = {
        {"Filter 1: ON LP Fc 1000 Hz\n", FilterType::LowPass,  0.70710678118654752, WidthMode::Q},
        {"Filter 1: ON HP Fc 1000 Hz\n", FilterType::HighPass, 0.70710678118654752, WidthMode::Q},
        {"Filter 1: ON BP Fc 1000 Hz\n", FilterType::BandPass, 0.70710678118654752, WidthMode::Q},
        {"Filter 1: ON NO Fc 1000 Hz\n", FilterType::Notch,    30.0,                WidthMode::Q},
    };
    for (const Case& c : cases) {
        const ApoParseResult r = parse_apo_config(c.text);
        CAPTURE(c.text);
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].type == c.type);
        CHECK(r.state.bands[0].width == doctest::Approx(c.width));
        CHECK(r.state.bands[0].width_mode == c.mode);
    }
}

TEST_CASE("a peaking filter with no Q is rejected, as upstream rejects it") {
    const ApoParseResult r = parse_apo_config("Filter 1: ON PK Fc 1000 Hz Gain 6 dB\n");
    CHECK(r.state.bands.empty());
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("the Room EQ Wizard thousands separator quirk is reproduced") {
    // REW writes 1000 Hz as "1.000". Upstream multiplies by a thousand when the
    // string is five or more characters with a period four from the end.
    const ApoParseResult r = parse_apo_config("Filter 1: ON PK Fc 1.000 Hz Gain 3 dB Q 1\n");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].fc == doctest::Approx(1000.0));

    // And a genuine decimal is left alone.
    const ApoParseResult s = parse_apo_config("Filter 1: ON PK Fc 1000.5 Hz Gain 3 dB Q 1\n");
    REQUIRE(s.state.bands.size() == 1);
    CHECK(s.state.bands[0].fc == doctest::Approx(1000.5));

    const ApoParseResult t = parse_apo_config("Filter 1: ON PK Fc 12.5 Hz Gain 3 dB Q 1\n");
    REQUIRE(t.state.bands.size() == 1);
    CHECK(t.state.bands[0].fc == doctest::Approx(12.5));
}

TEST_CASE("a comma decimal mark is normalised") {
    const ApoParseResult r = parse_apo_config("Filter 1: ON PK Fc 1000 Hz Gain 3,5 dB Q 1,4\n");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].gain_db == doctest::Approx(3.5));
    CHECK(r.state.bands[0].width == doctest::Approx(1.4));
}

TEST_CASE("Channel lines scope the bands that follow") {
    const char* text =
        "Channel: L\n"
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n"
        "Channel: R\n"
        "Filter 2: ON PK Fc 2000 Hz Gain -6 dB Q 1\n"
        "Channel: all\n"
        "Filter 3: ON PK Fc 4000 Hz Gain 2 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text);
    REQUIRE(r.state.bands.size() == 3);
    CHECK(r.state.bands[0].channels == (ChannelMask{1} << 0));
    CHECK(r.state.bands[1].channels == (ChannelMask{1} << 1));
    CHECK(r.state.bands[2].channels == kAllChannels);

    // Round trip preserves the scoping.
    const ApoParseResult again = parse_apo_config(format_apo_config(r.state));
    REQUIRE(again.state.bands.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(again.state.bands[i].channels == r.state.bands[i].channels);
    }
}

TEST_CASE("OFF and None filters are skipped") {
    const char* text =
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n"
        "Filter 2: OFF PK Fc 2000 Hz Gain 6 dB Q 1\n"
        "Filter 3: ON None\n"
        "Filter 4: ON PK Fc 3000 Hz Gain 6 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 2);
    CHECK(r.state.bands[0].fc == doctest::Approx(1000.0));
    CHECK(r.state.bands[1].fc == doctest::Approx(3000.0));
}

TEST_CASE("comments and blank lines are ignored") {
    const char* text =
        "# a comment\n"
        "\n"
        "   \n"
        "Preamp: -3 dB   # trailing comment\n"
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.warnings.empty());
    CHECK(r.state.preamp_db == doctest::Approx(-3.0));
    CHECK(r.state.bands.size() == 1);
}

TEST_CASE("unsupported directives are preserved rather than dropped") {
    const char* text =
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n"
        "Convolution: room.wav\n"
        "GraphicEQ: 25 -10; 40 -8\n"
        "Include: other.txt\n"
        "Copy: L=R R=L\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.state.bands.size() == 1);
    REQUIRE(r.unsupported.size() == 4);
    CHECK(r.unsupported[0].rfind("Convolution:", 0) == 0);
    CHECK(r.unsupported[3].rfind("Copy:", 0) == 0);
}

TEST_CASE("a Device line is emitted and can be scoped to one endpoint by GUID") {
    EqState s;
    Band b;
    b.type = FilterType::Peaking;
    b.fc = 1000.0;
    b.gain_db = -12.0;
    b.width = 1.0;
    s.bands.push_back(b);

    ApoFormatOptions opt;
    opt.device = apo_device_pattern_for_guid("6b7fbbae-dadf-48e3-91bb-9a9967a09feb");
    opt.header_comment = "written by Isotone";

    const std::string text = format_apo_config(s, opt);
    CHECK(text.find("# written by Isotone\n") == 0);
    CHECK(text.find("Device: {6b7fbbae-dadf-48e3-91bb-9a9967a09feb}\n") != std::string::npos);
    CHECK(text.find("Filter 1: ON PK Fc 1000 Hz Gain -12 dB Q 1\n") != std::string::npos);

    const ApoParseResult r = parse_apo_config(text);
    REQUIRE(r.devices.size() == 1);
    CHECK(r.devices[0] == "{6b7fbbae-dadf-48e3-91bb-9a9967a09feb}");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].gain_db == doctest::Approx(-12.0));
}

TEST_CASE("apo_device_pattern_for_guid adds braces when they are missing") {
    const std::string want = "{6b7fbbae-dadf-48e3-91bb-9a9967a09feb}";
    CHECK(apo_device_pattern_for_guid("6b7fbbae-dadf-48e3-91bb-9a9967a09feb") == want);
    CHECK(apo_device_pattern_for_guid("{6b7fbbae-dadf-48e3-91bb-9a9967a09feb}") == want);
    CHECK(apo_device_pattern_for_guid("  6b7fbbae-dadf-48e3-91bb-9a9967a09feb  ") == want);
    CHECK(apo_device_pattern_for_guid("").empty());
}

TEST_CASE("preamp round-trips including a zero") {
    EqState s;
    s.preamp_db = -7.25;
    Band b;
    b.type = FilterType::Peaking;
    b.fc = 500.0;
    b.gain_db = 2.0;
    b.width = 1.0;
    s.bands.push_back(b);

    const ApoParseResult r = parse_apo_config(format_apo_config(s));
    CHECK(r.state.preamp_db == doctest::Approx(-7.25));

    s.preamp_db = 0.0;
    const ApoParseResult z = parse_apo_config(format_apo_config(s));
    CHECK(z.state.preamp_db == doctest::Approx(0.0));
}

TEST_CASE("garbage does not crash the parser") {
    const char* text =
        "Filter\n"
        "Filter 1: ON\n"
        "Filter 2: ON PK\n"
        "Filter 3: ON PK Fc\n"
        "Filter 4: ON ZZ Fc 1000 Hz Gain 1 dB Q 1\n"
        "Preamp:\n"
        "Preamp: not a number dB\n"
        "Channel:\n"
        "Device:\n"
        ":::::\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.state.bands.empty());
    CHECK(r.warnings.size() >= 3);
}

TEST_CASE("every filter type survives a round trip through the text format") {
    EqState s;
    const FilterType types[] = {
        FilterType::Peaking,  FilterType::LowPass,   FilterType::HighPass,
        FilterType::BandPass, FilterType::Notch,     FilterType::AllPass,
        FilterType::LowShelf, FilterType::HighShelf,
    };
    double fc = 100.0;
    for (FilterType t : types) {
        Band b;
        b.type = t;
        b.fc = fc;
        b.gain_db = 4.0;
        b.width = 1.3;
        b.width_mode = WidthMode::Q;
        s.bands.push_back(b);
        fc *= 1.7;
    }

    const ApoParseResult r = parse_apo_config(format_apo_config(s));
    REQUIRE(r.warnings.empty());
    REQUIRE(r.state.bands.size() == s.bands.size());

    const std::vector<double> grid = log_grid(20.0, 20000.0, 256);
    std::vector<double> a(grid.size()), b(grid.size());
    magnitude_db(s, 0, grid.data(), grid.size(), kFs, a.data());
    magnitude_db(r.state, 0, grid.data(), grid.size(), kFs, b.data());
    for (size_t i = 0; i < grid.size(); ++i) {
        CAPTURE(grid[i]);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-9));
    }
}
