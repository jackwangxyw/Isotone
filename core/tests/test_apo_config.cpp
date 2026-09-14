// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Equalizer APO config format. The fixtures here are shaped like the files that
// actually exist in the wild: AutoEq exports, Room EQ Wizard exports, and what
// Peace writes.

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

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
    magnitude_db(r.state, 2, 0, 0, grid.data(), grid.size(), kFs, a.data());
    magnitude_db(again.state, 2, 0, 0, grid.data(), grid.size(), kFs, b.data());
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

namespace {

// Windows speaker masks (ksmedia.h).
constexpr uint32_t kSpeaker51Surround = 0x60F;    // FL FR FC LFE SL SR
constexpr uint32_t kSpeaker51Back     = 0x3F;     // FL FR FC LFE BL BR
constexpr uint32_t kSpeaker714        = 0x2D63F;  // 7.1 surround + TFL TFR TBL TBR

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

}  // namespace

TEST_CASE("channel names follow the device's speaker layout, as upstream ChannelHelper does") {
    CHECK(joined(apo_channel_names({2, default_speaker_mask(2)})) == "L R");
    CHECK(joined(apo_channel_names({6, kSpeaker51Surround})) == "L R C LFE SL SR");
    CHECK(joined(apo_channel_names({6, kSpeaker51Back})) == "L R C LFE RL RR");
    CHECK(joined(apo_channel_names({8, default_speaker_mask(8)})) == "L R C LFE RL RR SL SR");
    // Positions upstream has no name for, and channels past the mask, are numbered.
    CHECK(joined(apo_channel_names({12, kSpeaker714})) == "L R C LFE RL RR SL SR 9 10 11 12");
    // A layout with no mask gets upstream's default for its count (FilterEngine
    // applies getDefaultChannelMask before naming); a count with no default is
    // numbered.
    CHECK(joined(apo_channel_names({4, 0})) == "L R RL RR");
    CHECK(joined(apo_channel_names({3, 0})) == "1 2 3");

    // getDefaultChannelMask: only these counts have a default.
    CHECK(default_speaker_mask(1) == 0x4);
    CHECK(default_speaker_mask(2) == 0x3);
    CHECK(default_speaker_mask(4) == 0x33);
    CHECK(default_speaker_mask(6) == kSpeaker51Surround);
    CHECK(default_speaker_mask(8) == 0x63F);
    CHECK(default_speaker_mask(3) == 0);
}

TEST_CASE("SL on 5.1 is the fifth channel, whichever 5.1 the device is") {
    const char* text = "Channel: SL\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n";
    // 5.1 surround has SL itself; 5.1 back has RL there, and upstream accepts
    // SL for it because the position is unambiguous.
    for (uint32_t mask : {kSpeaker51Surround, kSpeaker51Back}) {
        CAPTURE(mask);
        const ApoParseResult r = parse_apo_config(text, {6, mask});
        CHECK(r.warnings.empty());
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].channels == (ChannelMask{1} << 4));
    }
    // On 7.1 it is the seventh.
    const ApoParseResult r71 = parse_apo_config(text);
    REQUIRE(r71.state.bands.size() == 1);
    CHECK(r71.state.bands[0].channels == (ChannelMask{1} << 6));

    // SUB is the old name for LFE.
    const ApoParseResult sub =
        parse_apo_config("Channel: SUB\nFilter 1: ON PK Fc 60 Hz Gain -6 dB Q 1\n", {6, kSpeaker51Back});
    REQUIRE(sub.state.bands.size() == 1);
    CHECK(sub.state.bands[0].channels == (ChannelMask{1} << 3));

    // The exporter names channel 5 by the same layout.
    EqState fifth;
    fifth.bands.push_back(r71.state.bands[0]);
    fifth.bands[0].channels = ChannelMask{1} << 4;
    ApoFormatOptions opt;
    opt.layout = {6, kSpeaker51Surround};
    CHECK(format_apo_config(fifth, opt).find("Channel: SL\n") != std::string::npos);
    opt.layout = {6, kSpeaker51Back};
    CHECK(format_apo_config(fifth, opt).find("Channel: RL\n") != std::string::npos);
}

TEST_CASE("numbered channels are valid up to the layout's channel count") {
    // A 7.1.4 config addresses its height channels by number.
    const ChannelLayout layout{12, kSpeaker714};
    const char* text =
        "Channel: 11\n"
        "Filter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n"
        "Channel: L 12\n"
        "Filter 2: ON PK Fc 2000 Hz Gain -3 dB Q 1\n"
        "Channel: 13\n"
        "Filter 3: ON PK Fc 3000 Hz Gain 2 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text, layout);
    REQUIRE(r.state.bands.size() == 2);
    CHECK(r.state.bands[0].channels == (ChannelMask{1} << 10));
    CHECK(r.state.bands[1].channels == ((ChannelMask{1} << 0) | (ChannelMask{1} << 11)));
    CHECK(r.warnings.size() == 2);   // channel 13 does not exist, so filter 3 is dropped

    ApoFormatOptions opt;
    opt.layout = layout;
    const ApoParseResult again = parse_apo_config(format_apo_config(r.state, opt), layout);
    CHECK(again.warnings.empty());
    REQUIRE(again.state.bands.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        CAPTURE(i);
        CHECK(again.state.bands[i].channels == r.state.bands[i].channels);
    }
}

TEST_CASE("a Channel line that selects nothing scopes the filters after it to nothing") {
    // Upstream's ChannelFilter starts from an empty selection and adds what it
    // recognises, so an unknown or out-of-range channel leaves the following
    // filters acting on no channel. Treating that as "all channels" would put
    // the filter on every speaker instead.
    const char* text =
        "Channel: 33\n"
        "Filter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n"
        "Channel: BOGUS\n"
        "Filter 2: ON PK Fc 2000 Hz Gain -6 dB Q 1\n"
        "Channel: all\n"
        "Filter 3: ON PK Fc 4000 Hz Gain 2 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text);
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].channels == kAllChannels);
    CHECK(r.state.bands[0].fc == doctest::Approx(4000.0));
    CHECK(r.warnings.size() >= 4);   // two unknown channels, two dropped filters

    SUBCASE("but one recognised channel is enough") {
        const ApoParseResult partial =
            parse_apo_config("Channel: R BOGUS\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n");
        REQUIRE(partial.state.bands.size() == 1);
        CHECK(partial.state.bands[0].channels == (ChannelMask{1} << 1));
        CHECK(partial.warnings.size() == 1);
    }
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

TEST_CASE("an OFF filter with its parameters is a disabled band; OFF or None alone is skipped") {
    // Upstream ignores every OFF line, so reading one as a disabled band plays
    // the same; the exporter writes disabled bands this way.
    const char* text =
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n"
        "Filter 2: OFF PK Fc 2000 Hz Gain 6 dB Q 1\n"
        "Filter 3: ON None\n"
        "Filter 4: ON PK Fc 3000 Hz Gain 6 dB Q 1\n"
        "Filter 5: OFF\n"
        "Filter 6: OFF None\n"
        "Filter 7: OFF PK\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 3);
    CHECK(r.state.bands[0].fc == doctest::Approx(1000.0));
    CHECK(r.state.bands[0].enabled);
    CHECK(r.state.bands[1].fc == doctest::Approx(2000.0));
    CHECK(r.state.bands[1].gain_db == doctest::Approx(6.0));
    CHECK_FALSE(r.state.bands[1].enabled);
    CHECK(r.state.bands[2].fc == doctest::Approx(3000.0));
    CHECK(r.state.bands[2].enabled);

    // An OFF line with parameters that do not make a filter is reported.
    CHECK_FALSE(parse_apo_config("Filter 1: OFF PK Fc 2000 Hz Gain 6 dB\n").ok());
}

TEST_CASE("disabled bands survive export and import") {
    EqState s;
    const FilterType types[] = {
        FilterType::Peaking,  FilterType::LowPass, FilterType::HighPass,  FilterType::BandPass,
        FilterType::Notch,    FilterType::AllPass, FilterType::LowShelf, FilterType::HighShelf,
    };
    double fc = 100.0;
    uint32_t id = 1;
    for (FilterType t : types) {
        for (WidthMode mode : {WidthMode::Q, WidthMode::BandwidthOct, WidthMode::SlopeDb}) {
            const bool shelf = t == FilterType::LowShelf || t == FilterType::HighShelf;
            // The forms the importer reads: bandwidth for all but shelves, a dB
            // slope for shelves.
            if ((mode == WidthMode::BandwidthOct && shelf) || (mode == WidthMode::SlopeDb && !shelf)) continue;
            Band b;
            b.id = id++;
            b.type = t;
            b.fc = fc;
            b.gain_db = 4.0;
            b.width = mode == WidthMode::SlopeDb ? 9.0 : 1.3;
            b.width_mode = mode;
            b.shelf_corner = shelf;
            b.channels = (id % 2) != 0 ? kAllChannels : ChannelMask{2};
            b.enabled = (id % 3) != 0;
            s.bands.push_back(b);
            fc *= 1.3;
        }
    }
    const std::string text = format_apo_config(s);
    CAPTURE(text);
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.warnings.empty());
    REQUIRE(r.state.bands.size() == s.bands.size());
    // The exporter groups bands by channel mask; compare by frequency.
    for (const Band& want : s.bands) {
        CAPTURE(want.id);
        const auto it = std::find_if(r.state.bands.begin(), r.state.bands.end(),
                                     [&](const Band& b) { return std::abs(b.fc - want.fc) < 1e-6; });
        REQUIRE(it != r.state.bands.end());
        CHECK(it->enabled == want.enabled);
        CHECK(it->type == want.type);
        CHECK(it->width_mode == want.width_mode);
        CHECK(it->width == doctest::Approx(want.width));
        CHECK(it->shelf_corner == want.shelf_corner);
        CHECK(it->channels == want.channels);
        if (want.type == FilterType::Peaking || want.type == FilterType::LowShelf ||
            want.type == FilterType::HighShelf) {
            CHECK(it->gain_db == doctest::Approx(want.gain_db));
        }
    }
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

TEST_CASE("every line the import does not apply is reported as a warning, once") {
    // The UI lists the warnings as the import's skipped lines. An AutoEq
    // GraphicEQ file imported as a flat preset with nothing reported.
    const char* text =
        "Preamp: -3 dB\n"                          // 1
        "GraphicEQ: 25 -10; 40 -8\n"               // 2
        "Include: other.txt\n"                     // 3
        "Delay: 10 ms\n"                           // 4
        "Copy: L=R R=L\n"                          // 5
        "Convolution: room.wav\n"                  // 6
        "VSTPlugin: Library plugin.dll\n"          // 7
        "Loudness: on\n"                           // 8
        "If: sampleRate == 44100\n"                // 9
        "EndIf:\n"                                 // 10
        "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.state.bands.size() == 1);
    CHECK(r.state.preamp_db == doctest::Approx(-3.0));
    CHECK(r.unsupported.size() == 9);
    std::vector<size_t> lines;
    for (const ApoParseMessage& w : r.warnings) lines.push_back(w.line);
    CHECK(lines == std::vector<size_t>{2, 3, 4, 5, 6, 7, 8, 9, 10});
    CHECK_FALSE(r.ok());
}

TEST_CASE("a filter line with a long run of whitespace reads as a short one, and quickly") {
    // Visual C++'s std::regex threw error_complexity for 600 spaces between PK
    // and Fc, which nothing caught; libstdc++ took 78 s for 50,000, backtracking
    // through the run once per starting position (review 2026-09-13).
    const auto start = std::chrono::steady_clock::now();
    for (size_t n : {600u, 10000u}) {
        for (char c : {' ', '\t'}) {
            const std::string ws(n, c);
            const std::string text = "Filter 1: ON PK" + ws + "Fc 100" + ws + "Hz" + ws + "Gain -3 dB" + ws + "Q 1\n";
            CAPTURE(n);
            CAPTURE(static_cast<int>(c));
            ApoParseResult r;
            CHECK_NOTHROW(r = parse_apo_config(text));
            CHECK(r.warnings.empty());
            REQUIRE(r.state.bands.size() == 1);
            CHECK(r.state.bands[0].fc == 100.0);
            CHECK(r.state.bands[0].gain_db == -3.0);
            CHECK(r.state.bands[0].width == 1.0);
            // A line with no filter in it is still only a warning.
            ApoParseResult none;
            CHECK_NOTHROW(none = parse_apo_config("Filter 1: ON PK" + ws + "x\n"));
            CHECK(none.state.bands.empty());
            CHECK(none.warnings.size() == 1);
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CAPTURE(seconds);
    CHECK(seconds < 2.0);

}

TEST_CASE("a filter line longer than any filter is skipped before it is matched") {
    // A million digits in Fc: Visual C++ threw error_stack, which was caught,
    // but libstdc++ 13 matched the repeated group recursively, one frame per
    // character, and the CI job crashed with SIGSEGV (2026-09-14). Upstream's own
    // Visual C++ regex gives up on such a line too, so skipping it changes no
    // result.
    for (size_t digits : {5000u, 1000000u}) {
        CAPTURE(digits);
        const std::string text = "Filter 1: ON PK Fc " + std::string(digits, '1') + " Hz Gain -3 dB Q 1\n" +
                                 "Filter 2: ON PK Fc 200 Hz Gain -3 dB Q 1\n";
        ApoParseResult r;
        CHECK_NOTHROW(r = parse_apo_config(text));
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].fc == 200.0);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].line == 1);
        CHECK(r.warnings[0].text.find("longer") != std::string::npos);
    }
    // The longest line a real preset writes still reads.
    const std::string longest = "Filter 1: ON LSC 12 dB Fc " + std::string(40, '1') + ".5 Hz Gain -12.345678901234 dB Q 0.707106781187\n";
    CHECK(parse_apo_config(longest).state.bands.size() == 1);
}

TEST_CASE("a channel count larger than a stream can carry is bounded, with a warning") {
    // WAVEFORMATEX counts channels in a 16-bit field. A region value of
    // 0xFFFFFFFF asked for a name per channel: about 200 GB.
    const ChannelLayout huge{1000000, 0};
    CHECK(apo_channel_names(huge).size() == 65535);
    CHECK(apo_channel_names({65535, 0}).size() == 65535);
    const ApoParseResult r = parse_apo_config("Preamp: -3 dB\n", huge);
    CHECK(r.state.preamp_db == -3.0);
    CHECK(r.warnings.size() == 1);
    EqState mute;
    mute.mute = true;
    ApoFormatOptions options;
    options.layout = huge;
    const std::string text = format_apo_config(mute, options);
    CHECK(std::count(text.begin(), text.end(), '=') == 65535);
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
    magnitude_db(s, 2, 0, 0, grid.data(), grid.size(), kFs, a.data());
    magnitude_db(r.state, 2, 0, 0, grid.data(), grid.size(), kFs, b.data());
    for (size_t i = 0; i < grid.size(); ++i) {
        CAPTURE(grid[i]);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-9));
    }
}

TEST_CASE("channel trims survive a round trip alongside the preamp") {
    EqState s;
    s.preamp_db = -6.0;
    s.channel_gain_db[1] = -3.0;
    s.channel_gain_db[5] = 1.5;
    const ApoParseResult r = parse_apo_config(format_apo_config(s));
    CHECK(r.warnings.empty());
    CHECK(r.state.preamp_db == doctest::Approx(-6.0));
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        CAPTURE(c);
        CHECK(r.state.channel_gain_db[c] == doctest::Approx(s.channel_gain_db[c]));
    }
}

TEST_CASE("Preamp lines add up, and inside a Channel scope they are trims") {
    // Upstream makes each Preamp line its own gain stage on the channels
    // selected at that point (PreampFilterFactory: "Adjusting preamp by").
    const char* text =
        "Preamp: -2 dB\n"
        "Preamp: -1 dB\n"
        "Channel: L\n"
        "Preamp: -3 dB\n"
        "Channel: L R\n"
        "Preamp: 1 dB\n"
        "Channel: all\n"
        "Preamp: -0.5 dB\n";
    const ApoParseResult r = parse_apo_config(text);
    CHECK(r.warnings.empty());
    CHECK(r.state.preamp_db == doctest::Approx(-3.5));
    CHECK(r.state.channel_gain_db[0] == doctest::Approx(-2.0));
    CHECK(r.state.channel_gain_db[1] == doctest::Approx(1.0));
    CHECK(r.state.channel_gain_db[2] == doctest::Approx(0.0));

    SUBCASE("a trim for a channel with no trim slot, or for no channel, is reported") {
        const ApoParseResult wide = parse_apo_config(
            "Channel: 11\nPreamp: -3 dB\nChannel: BOGUS\nPreamp: -3 dB\n", {12, kSpeaker714});
        CHECK(wide.state.preamp_db == doctest::Approx(0.0));
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            CHECK(wide.state.channel_gain_db[c] == doctest::Approx(0.0));
        }
        CHECK(wide.warnings.size() == 3);   // channel 11 trim, BOGUS, trim on nothing
    }
}

TEST_CASE("a layout with no speaker mask resolves channel names by the default mask") {
    const ApoParseResult r = parse_apo_config("Channel: R\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n", {2, 0});
    CHECK(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].channels == 0x2u);
}

TEST_CASE("a non-breaking space thousands separator in Fc is removed, as upstream does") {
    // BiQuadFilterFactory::getFreq, "for locales utilizing non-breaking space":
    // as UTF-8, or as the lone byte upstream's code-page fallback decodes.
    for (const char* text : {"Filter 1: ON PK Fc 1\xC2\xA0" "200 Hz Gain -6 dB Q 1\n",
                             "Filter 1: ON PK Fc 1\xA0" "200 Hz Gain -6 dB Q 1\n"}) {
        const ApoParseResult r = parse_apo_config(text);
        CHECK(r.warnings.empty());
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].fc == doctest::Approx(1200.0));
    }
}

TEST_CASE("filter lines are read with upstream's patterns: case-sensitive, units required") {
    // Each of these is a line Equalizer APO plays differently from a lenient
    // reading; an import must measure the same as it does there.
    struct Case { const char* text; bool band; };
    const Case dropped[] = {
        {"Filter 1: on PK Fc 100 Hz Gain -6 dB Q 1\n", false},    // ON is case-sensitive
        {"Filter 1: ON pk Fc 100 Hz Gain -6 dB Q 1\n", false},    // so are the types
        {"Filter 1: ON MODAL Fc 100 Hz Gain -6 dB Q 1\n", false},
        {"Filter 1: ON PK fc 100 Hz Gain -6 dB Q 1\n", false},
        {"Filter 1: ON PK Fc 100 hz Gain -6 dB Q 1\n", false},
        {"Filter 1: ON PK Fc 100 Gain -6 dB Q 1\n", false},       // no Hz: no frequency
        {"Filter 1: ON PK Fc 100 Hz Gain -6 Q 1\n", false},       // no dB: no gain
        {"Filter 1: ON PK Fc 1 000 Hz Gain -6 dB Q 1\n", false},  // a plain space is no separator
        {"Channel:\tR\nFilter 1: ON PK Fc 100 Hz Gain -6 dB Q 1\n", false},   // words split on spaces only
    };
    for (const Case& c : dropped) {
        CAPTURE(c.text);
        const ApoParseResult r = parse_apo_config(c.text);
        CHECK(r.state.bands.empty());
        CHECK_FALSE(r.warnings.empty());
    }

    SUBCASE("whitespace upstream's patterns allow") {
        const ApoParseResult r = parse_apo_config(
            "Filter 1: ON PK Fc 100 H z Gain -6 dB BW  Oct 1\n"
            "Filter 2: ON PK Fc200Hz Gain-3dB Q2\n");
        CHECK(r.warnings.empty());
        REQUIRE(r.state.bands.size() == 2);
        CHECK(r.state.bands[0].width_mode == WidthMode::BandwidthOct);
        CHECK(r.state.bands[0].width == doctest::Approx(1.0));
        CHECK(r.state.bands[1].fc == doctest::Approx(200.0));
        CHECK(r.state.bands[1].gain_db == doctest::Approx(-3.0));
        CHECK(r.state.bands[1].width == doctest::Approx(2.0));
    }
    SUBCASE("a shelf with both a dB slope and a Q uses the slope, which upstream reads last") {
        const ApoParseResult r = parse_apo_config("Filter 1: ON LS 6 dB Fc 100 Hz Gain 6 dB Q 0.7\n");
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].width_mode == WidthMode::SlopeDb);
        CHECK(r.state.bands[0].width == doctest::Approx(6.0));
        CHECK(r.state.bands[0].shelf_corner);
    }
    SUBCASE("a negative width, which upstream designs unstable, is reported and dropped") {
        const ApoParseResult r = parse_apo_config("Filter 1: ON LPQ Fc 100 Hz Q -1\n");
        CHECK(r.state.bands.empty());
        CHECK_FALSE(r.warnings.empty());
    }
}

TEST_CASE("only a line starting with # is a comment, as upstream reads it") {
    const ApoParseResult r = parse_apo_config(
        "Include: EQ #2.txt\n"
        "Filter 1: ON PK Fc 100 Hz # Gain 3 dB Q 1\n"
        "  # Filter 2: ON PK Fc 200 Hz Gain 3 dB Q 1\n"
        "#no colon either\n");
    REQUIRE(r.warnings.size() == 1);   // the Include, which is not applied
    CHECK(r.warnings[0].line == 1);
    REQUIRE(r.unsupported.size() == 1);
    CHECK(r.unsupported[0] == "Include: EQ #2.txt");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].gain_db == doctest::Approx(3.0));
}

TEST_CASE("conditional sections are not merged silently") {
    SUBCASE("a Stage section for capture only is skipped; playback stages are read") {
        const ApoParseResult r = parse_apo_config(
            "Stage: capture\n"
            "Preamp: -6 dB\n"
            "Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1\n"
            "Stage: pre-mix post-mix\n"
            "Filter 2: ON PK Fc 2000 Hz Gain 6 dB Q 1\n"
            "Stage: post-mix\n"
            "Filter 3: ON PK Fc 3000 Hz Gain 6 dB Q 1\n");
        CHECK(r.state.preamp_db == 0.0);
        REQUIRE(r.state.bands.size() == 2);
        CHECK(r.state.bands[0].fc == doctest::Approx(2000.0));
        CHECK(r.state.bands[1].fc == doctest::Approx(3000.0));
        CHECK_FALSE(r.warnings.empty());   // the skipped section is reported
    }
    SUBCASE("If and Else branches, which only Equalizer APO can evaluate, are reported") {
        const ApoParseResult r = parse_apo_config(
            "If: sampleRate == 44100\nPreamp: -3 dB\nElse:\nPreamp: -4 dB\nEndIf:\n");
        CHECK_FALSE(r.ok());
    }
    SUBCASE("Device sections for particular devices are reported; Device: all is not") {
        CHECK_FALSE(parse_apo_config("Device: Speakers\nPreamp: -6 dB\nDevice: Headphones\nPreamp: -3 dB\n").ok());
        CHECK(parse_apo_config("Device: all\nPreamp: -6 dB\n").ok());
    }
}

namespace {

Band band_on(uint32_t id, double fc, ChannelMask channels) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = -6.0;
    b.channels = channels;
    return b;
}

ChannelMask bit(uint32_t channel) { return ChannelMask{1} << channel; }

}  // namespace

TEST_CASE("a parsed state records the layout its channel indexes are for") {
    const ApoParseResult r = parse_apo_config("Channel: SL\nPreamp: -3 dB\n", {6, kSpeaker51Surround});
    CHECK(r.state.layout_channels == 6);
    CHECK(r.state.layout_speaker_mask == kSpeaker51Surround);
    CHECK(r.state.channel_gain_db[4] == -3.0);
    const ApoParseResult huge = parse_apo_config("", {1000000, 0});
    CHECK(huge.state.layout_channels == kMaxApoChannels);
}

TEST_CASE("remap_channels moves every per-channel value to the same speaker on another layout") {
    // 7.1 surround: L R C LFE RL RR SL SR.
    EqState s;
    s.layout_channels = 8;
    s.layout_speaker_mask = 0x63F;
    s.bands.push_back(band_on(1, 1000, bit(6)));            // SL
    s.bands.push_back(band_on(2, 2000, bit(1) | bit(6)));   // R SL
    s.bands.push_back(band_on(3, 3000, kAllChannels));
    s.channel_gain_db[4] = -2.5;                            // RL
    s.speakers.delay_ms[4] = 1.5;                           // RL
    s.speakers.inverted = bit(7);                           // SR
    s.speakers.muted = bit(2);                              // C
    s.speakers.small_speakers = bit(0);                     // L

    SUBCASE("5.1 surround: each lands on its speaker, and RL on SL, which stands in for it") {
        EqState r = s;
        remap_channels(&r, {6, kSpeaker51Surround});   // L R C LFE SL SR
        REQUIRE(r.bands.size() == 3);
        CHECK(r.bands[0].channels == bit(4));
        CHECK(r.bands[0].enabled);
        CHECK(r.bands[1].channels == (bit(1) | bit(4)));
        CHECK(r.bands[2].channels == kAllChannels);
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            CAPTURE(c);
            CHECK(r.channel_gain_db[c] == (c == 4 ? -2.5 : 0.0));
            CHECK(r.speakers.delay_ms[c] == (c == 4 ? 1.5 : 0.0));
        }
        CHECK(r.speakers.inverted == bit(5));
        CHECK(r.speakers.muted == bit(2));
        CHECK(r.speakers.small_speakers == bit(0));
        CHECK(r.layout_channels == 6);
        CHECK(r.layout_speaker_mask == kSpeaker51Surround);
    }
    SUBCASE("stereo: speakers it lacks are dropped, and a band only on them plays nowhere") {
        EqState r = s;
        remap_channels(&r, {2, 0x3});
        REQUIRE(r.bands.size() == 3);
        CHECK_FALSE(r.bands[0].enabled);
        CHECK(r.bands[0].channels != kAllChannels);
        CHECK(r.bands[1].enabled);
        CHECK(r.bands[1].channels == bit(1));
        CHECK(r.bands[2].enabled);
        CHECK(r.bands[2].channels == kAllChannels);
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            CAPTURE(c);
            CHECK(r.channel_gain_db[c] == 0.0);
            CHECK(r.speakers.delay_ms[c] == 0.0);
        }
        CHECK(r.speakers.inverted == 0);
        CHECK(r.speakers.muted == 0);
        CHECK(r.speakers.small_speakers == bit(0));
        CHECK(r.layout_channels == 2);
    }
    SUBCASE("5.1 surround to 5.1 back: SL and SR become RL and RR") {
        EqState r;
        r.layout_channels = 6;
        r.layout_speaker_mask = kSpeaker51Surround;
        r.channel_gain_db[4] = -1.0;
        r.speakers.muted = bit(5);
        remap_channels(&r, {6, kSpeaker51Back});   // L R C LFE RL RR
        CHECK(r.channel_gain_db[4] == -1.0);
        CHECK(r.speakers.muted == bit(5));
    }
}

TEST_CASE("remap_channels combines values that land on one channel as Equalizer APO's lines do") {
    // 7.1 SL and RL both resolve to SL on 5.1 surround.
    EqState s;
    s.layout_channels = 8;
    s.layout_speaker_mask = 0x63F;
    s.channel_gain_db[4] = -2.0;
    s.channel_gain_db[6] = -3.0;
    s.speakers.delay_ms[4] = 1.0;
    s.speakers.delay_ms[6] = 2.0;
    s.speakers.inverted = bit(4) | bit(6);
    s.speakers.muted = bit(6);
    s.bands.push_back(band_on(1, 1000, bit(4) | bit(6)));
    remap_channels(&s, {6, kSpeaker51Surround});
    CHECK(s.channel_gain_db[4] == -5.0);
    CHECK(s.speakers.delay_ms[4] == 3.0);
    CHECK(s.speakers.inverted == bit(4));
    CHECK(s.speakers.muted == bit(4));
    CHECK(s.bands[0].channels == bit(4));
}

TEST_CASE("remap_channels keeps a numbered channel's index, as Equalizer APO names it by number") {
    // 7.1.4 names its height channels 9 to 12; a layout that numbers the same
    // positions keeps them, one with fewer channels drops them.
    EqState s;
    s.layout_channels = 12;
    s.layout_speaker_mask = kSpeaker714;
    s.bands.push_back(band_on(1, 1000, bit(10)));
    s.bands.push_back(band_on(2, 2000, bit(10) | bit(7)));
    EqState wider = s;
    remap_channels(&wider, {12, 0x63F});
    CHECK(wider.bands[0].channels == bit(10));
    CHECK(wider.bands[0].enabled);
    EqState narrower = s;
    remap_channels(&narrower, {8, 0x63F});
    CHECK_FALSE(narrower.bands[0].enabled);
    CHECK(narrower.bands[1].channels == bit(7));

    // A position upstream has no name for is numbered too: 7.1 wide's front left
    // of centre is channel 7, which is SL's index on 7.1 surround.
    EqState wide;
    wide.layout_channels = 8;
    wide.layout_speaker_mask = 0xFF;   // L R C LFE RL RR FLC FRC
    wide.channel_gain_db[6] = -4.0;
    remap_channels(&wide, {8, 0x63F});
    CHECK(wide.channel_gain_db[6] == -4.0);
}

TEST_CASE("remap_channels leaves a state alone when its layout is unspecified or the same") {
    EqState s;
    s.bands.push_back(band_on(1, 1000, bit(6)));
    s.channel_gain_db[6] = -3.0;
    EqState unspecified = s;
    remap_channels(&unspecified, {2, 0x3});
    CHECK(unspecified.bands[0].enabled);
    CHECK(unspecified.bands[0].channels == bit(6));
    CHECK(unspecified.channel_gain_db[6] == -3.0);
    CHECK(unspecified.layout_channels == 0);

    // A mask of 0 is the default for its count.
    EqState same = s;
    same.layout_channels = 8;
    same.layout_speaker_mask = 0;
    remap_channels(&same, {8, 0x63F});
    CHECK(same.bands[0].channels == bit(6));
    CHECK(same.channel_gain_db[6] == -3.0);

    EqState to_nothing = same;
    remap_channels(&to_nothing, {0, 0});
    CHECK(to_nothing.bands[0].channels == bit(6));
    CHECK(to_nothing.layout_channels == 8);
}

TEST_CASE("remap_channels agrees with writing a state for one layout and reading it on another") {
    // format_apo_config names channels for the layout it writes for, and
    // parse_apo_config resolves the names as upstream does on the layout it
    // reads for. The remap must land every band and trim where that does.
    const ChannelLayout layouts[] = {{1, 0x4},   {2, 0x3},   {3, 0x7},   {4, 0x33},  {4, 0x107},
                                     {6, 0x60F}, {6, 0x3F},  {8, 0x63F}, {8, 0xFF},  {12, kSpeaker714},
                                     {3, 0},     {10, 0x63F}};
    for (const ChannelLayout& from : layouts) {
        EqState s;
        s.layout_channels = from.channels;
        s.layout_speaker_mask = from.speaker_mask;
        for (uint32_t c = 0; c < std::min(from.channels, kMaxChannels); ++c) {
            s.bands.push_back(band_on(c + 1, 100.0 * (c + 1), bit(c)));
            s.channel_gain_db[c] = -(c + 1.0);
        }
        s.bands.push_back(band_on(100, 5000, bit(0) | bit(from.channels - 1)));
        ApoFormatOptions options;
        options.layout = from;
        const std::string text = format_apo_config(s, options);
        for (const ChannelLayout& to : layouts) {
            CAPTURE(from.channels);
            CAPTURE(from.speaker_mask);
            CAPTURE(to.channels);
            CAPTURE(to.speaker_mask);
            const ApoParseResult parsed = parse_apo_config(text, to);
            EqState r = s;
            remap_channels(&r, to);
            std::vector<Band> enabled;
            for (const Band& b : r.bands) {
                if (b.enabled) enabled.push_back(b);
            }
            // The parser keeps bands in the order the text groups them.
            std::sort(enabled.begin(), enabled.end(), [](const Band& a, const Band& b) { return a.fc < b.fc; });
            std::vector<Band> read = parsed.state.bands;
            std::sort(read.begin(), read.end(), [](const Band& a, const Band& b) { return a.fc < b.fc; });
            REQUIRE(read.size() == enabled.size());
            for (size_t i = 0; i < enabled.size(); ++i) {
                CAPTURE(i);
                CHECK(read[i].fc == enabled[i].fc);
                CHECK(read[i].channels == enabled[i].channels);
            }
            for (uint32_t c = 0; c < kMaxChannels; ++c) {
                CAPTURE(c);
                CHECK(parsed.state.channel_gain_db[c] == r.channel_gain_db[c]);
            }
        }
    }
}

TEST_CASE("remap_channels works in place, so the audio thread can run it") {
    // Every other working value is on the stack; the bands are edited where
    // they are, so the vector keeps its storage.
    EqState s;
    s.layout_channels = 8;
    s.layout_speaker_mask = 0x63F;
    s.bands.reserve(64);
    for (uint32_t i = 0; i < 64; ++i) s.bands.push_back(band_on(i + 1, 100.0 + i, bit(i % 8)));
    const Band* data = s.bands.data();
    const size_t capacity = s.bands.capacity();
    remap_channels(&s, {6, kSpeaker51Surround});
    CHECK(s.bands.data() == data);
    CHECK(s.bands.capacity() == capacity);
    CHECK(s.bands.size() == 64);
}
