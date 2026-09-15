// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The UI's backend without Qt: spectrum analysis, the state an output gets, and
// DeviceLink against a Local\ region and a sandbox Equalizer APO directory.

#include "doctest.h"

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "devicelink.h"
#include "isotone/param_block.h"
#include "isotone/response.h"
#include "output_state.h"
#include "shared_mapping.h"
#include "spectrum.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

constexpr double kPi = 3.14159265358979323846;

Band peaking(uint32_t id, double fc, double gain, double q) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = gain;
    b.width = q;
    return b;
}

}  // namespace

TEST_CASE("fft of a cosine puts half its amplitude in its bin and its mirror") {
    std::vector<std::complex<double>> x(16);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::cos(2.0 * kPi * 3.0 * static_cast<double>(i) / 16.0);
    fft(x);
    for (size_t b = 0; b < x.size(); ++b) {
        CAPTURE(b);
        CHECK(std::abs(x[b]) == doctest::Approx(b == 3 || b == 13 ? 8.0 : 0.0).epsilon(1e-9).scale(1.0));
    }
}

TEST_CASE("a sine on a bin reads its level in dBFS, and silence elsewhere") {
    SpectrumAnalyzer a;
    const double rate = 48000.0;
    const double bin_hz = rate / SpectrumAnalyzer::kFftSize;
    const double f = 170 * bin_hz;   // 996 Hz, on bin 170
    std::vector<float> stereo(2 * 12000);
    for (size_t i = 0; i < 12000; ++i) {
        const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(i) / rate));
        stereo[2 * i] = v;
        stereo[2 * i + 1] = v;
    }
    CHECK(a.bin_db()[170] == SpectrumAnalyzer::kFloorDb);
    a.push(stereo.data(), 12000, 2);
    a.update(rate, 2.0);   // long enough for the attack to settle
    CHECK(a.bin_db()[170] == doctest::Approx(20.0 * std::log10(0.5)).epsilon(0.001));
    CHECK(a.bin_db()[1000] < -80.0);

    const double freqs[] = {f / 1.01, f, f * 1.01};
    double out[3];
    a.levels_at(freqs, 3, out);
    CHECK(out[1] == doctest::Approx(-6.02).epsilon(0.01));

    SUBCASE("it releases towards silence over its release time, not at once") {
        std::vector<float> quiet(2 * SpectrumAnalyzer::kFftSize, 0.0f);
        a.push(quiet.data(), SpectrumAnalyzer::kFftSize, 2);
        a.update(rate, 1.0 / 60.0);
        // One frame of silence: 16.7 ms of a 300 ms release in power, 0.24 dB.
        CHECK(a.bin_db()[170] == doctest::Approx(-6.02 - 0.24).epsilon(0.005));
        a.push(quiet.data(), SpectrumAnalyzer::kFftSize, 2);
        a.update(rate, 0.3);
        CHECK(a.bin_db()[170] == doctest::Approx(-6.02 - 0.24 - 4.34).epsilon(0.005));
    }
    SUBCASE("channels are mixed: a sine on one of two channels reads 6 dB lower") {
        SpectrumAnalyzer one;
        for (size_t i = 0; i < 12000; ++i) stereo[2 * i + 1] = 0.0f;
        one.push(stereo.data(), 12000, 2);
        one.update(rate, 2.0);
        CHECK(one.bin_db()[170] == doctest::Approx(20.0 * std::log10(0.25)).epsilon(0.001));
    }
}

TEST_CASE("balance turns down only the opposite side, and mutes it at the end") {
    EqState s;
    apply_balance(-0.5, 2, &s);
    CHECK(s.channel_gain_db[0] == 0.0);
    CHECK(s.channel_gain_db[1] == doctest::Approx(-6.0206).epsilon(1e-4));
    CHECK(s.speakers.muted == 0);

    apply_balance(0.2, 2, &s);
    CHECK(s.channel_gain_db[0] == doctest::Approx(20.0 * std::log10(0.8)));
    CHECK(s.channel_gain_db[1] == 0.0);

    apply_balance(1.0, 2, &s);
    CHECK(s.channel_gain_db[0] == 0.0);
    CHECK(s.speakers.muted == 0x1);

    apply_balance(0.0, 2, &s);
    CHECK(s.channel_gain_db[0] == 0.0);
    CHECK(s.channel_gain_db[1] == 0.0);
    CHECK(s.speakers.muted == 0);

    SUBCASE("not on other layouts") {
        EqState surround;
        apply_balance(-1.0, 8, &surround);
        CHECK(surround.speakers.muted == 0);
        CHECK(surround.channel_gain_db[1] == 0.0);
    }
}

TEST_CASE("the balance a state holds reads back as apply_balance wrote it") {
    for (double b : {-1.0, -0.7, -0.2, 0.0, 0.3, 1.0}) {
        CAPTURE(b);
        EqState s;
        apply_balance(b, 2, &s);
        CHECK(balance_from_state(s, 2) == doctest::Approx(b).epsilon(1e-9).scale(1.0));
        clear_balance(2, &s);
        CHECK(balance_from_state(s, 2) == 0.0);
    }
    // Trims on both sides are not a balance.
    EqState both;
    both.channel_gain_db[0] = -3;
    both.channel_gain_db[1] = -3;
    CHECK(balance_from_state(both, 2) == 0.0);
}

TEST_CASE("state_for_output sets the output's layout") {
    EqState edited;
    edited.bands.push_back(peaking(1, 1000, -6, 1));
    const EqState s = state_for_output(edited, -1.0, OutputLayout{2, 0x3, 48000});
    CHECK(s.layout_channels == 2);
    CHECK(s.layout_speaker_mask == 0x3);
    CHECK(s.speakers.muted == 0x2);
    CHECK(s.bands.size() == 1);
}

TEST_CASE("DeviceLink writes a native output's region, and finds it again once the engine creates it") {
    const std::wstring ns = L"Local\\IsotoneUiTest." + std::to_wstring(GetCurrentProcessId()) + L".";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-00000000abcd}";
    DeviceLink link(ns);
    link.set_target(OutputTarget{guid, Backend::native, OutputLayout{2, 0x3, 48000}});

    EqState state;
    state.bands.push_back(peaking(1, 1000, -6, 1));
    state.layout_channels = 2;
    CHECK(link.apply(state) == ERROR_FILE_NOT_FOUND);   // no engine yet
    CHECK_FALSE(link.region_open());

    isotone::win::SharedMapping engine;
    REQUIRE(engine.create_or_open(isotone::win::mapping_name(ns.c_str(), guid)) == ERROR_SUCCESS);
    // The next attempt waits out the reopen interval, then finds the region.
    Sleep(1100);
    CHECK(link.apply(state) == ERROR_SUCCESS);
    CHECK(link.region_open());
    CHECK(link.take_region_opened());
    CHECK_FALSE(link.take_region_opened());

    EqState read;
    from_param_block(*engine.params(), &read);
    REQUIRE(read.bands.size() == 1);
    CHECK(read.bands[0].gain_db == doctest::Approx(-6.0));
    CHECK(read.layout_channels == 2);

    state.bands[0].gain_db = -3;
    CHECK(link.commit(state) == ERROR_SUCCESS);
    from_param_block(*engine.params(), &read);
    CHECK(read.bands[0].gain_db == doctest::Approx(-3.0));

    // What the engine plays is what a newly opened link loads.
    DeviceLink other(ns);
    other.set_target(OutputTarget{guid, Backend::native, OutputLayout{2, 0x3, 48000}});
    EqState loaded;
    REQUIRE(other.load_current(&loaded));
    REQUIRE(loaded.bands.size() == 1);
    CHECK(loaded.bands[0].gain_db == doctest::Approx(-3.0));
}

TEST_CASE("every filter type reaches either backend and loads back with the same response") {
    using T = FilterType;
    struct Case {
        const char* name;
        Band band;
    };
    const auto make = [](T type, double fc, double gain, double width, WidthMode mode, bool corner) {
        Band b = peaking(1, fc, gain, width);
        b.type = type;
        b.width_mode = mode;
        b.shelf_corner = corner;
        return b;
    };
    const std::vector<Case> cases = {
        {"peak Q", make(T::Peaking, 1000, 6, 1.41, WidthMode::Q, false)},
        {"peak BW", make(T::Peaking, 3000, -9, 1.5, WidthMode::BandwidthOct, false)},
        {"low shelf Q", make(T::LowShelf, 200, 6, 0.707, WidthMode::Q, false)},
        {"low shelf slope, corner", make(T::LowShelf, 200, 6, 12, WidthMode::SlopeDb, true)},
        {"high shelf Q", make(T::HighShelf, 5000, -6, 0.707, WidthMode::Q, false)},
        {"high shelf slope, corner", make(T::HighShelf, 5000, -6, 6, WidthMode::SlopeDb, true)},
        {"low pass", make(T::LowPass, 2000, 0, 0.707, WidthMode::Q, false)},
        {"high pass", make(T::HighPass, 200, 0, 0.707, WidthMode::Q, false)},
        {"band pass", make(T::BandPass, 1000, 0, 2, WidthMode::Q, false)},
        {"notch", make(T::Notch, 1000, 0, 5, WidthMode::Q, false)},
        {"all pass", make(T::AllPass, 1000, 0, 0.707, WidthMode::Q, false)},
    };
    const std::vector<double> grid = log_grid(20.0, 20000.0, 256);
    const OutputLayout layout{2, 0x3, 48000};
    // Magnitude and phase on both channels agree within `tolerance` (dB and
    // degrees), where the output is not so far down that phase means nothing.
    const auto same_response = [&](const EqState& want, const EqState& got, double tolerance) {
        for (uint32_t ch = 0; ch < 2; ++ch) {
            std::vector<double> wm(grid.size()), gm(grid.size()), wp(grid.size()), gp(grid.size());
            magnitude_db(want, 2, 0x3, ch, grid.data(), grid.size(), 48000, wm.data());
            magnitude_db(got, 2, 0x3, ch, grid.data(), grid.size(), 48000, gm.data());
            phase_deg(want, 2, 0x3, ch, grid.data(), grid.size(), 48000, wp.data());
            phase_deg(got, 2, 0x3, ch, grid.data(), grid.size(), 48000, gp.data());
            for (size_t i = 0; i < grid.size(); ++i) {
                if (!std::isfinite(wm[i]) || wm[i] < -60.0) continue;
                CAPTURE(ch);
                CAPTURE(grid[i]);
                CHECK(std::abs(gm[i] - wm[i]) < tolerance);
                const double dp = std::remainder(gp[i] - wp[i], 360.0);
                CHECK(std::abs(dp) < tolerance);
            }
        }
    };

    const std::wstring ns = L"Local\\IsotoneUiFilters." + std::to_wstring(GetCurrentProcessId()) + L".";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-0000000fa17e}";
    isotone::win::SharedMapping engine;
    REQUIRE(engine.create_or_open(isotone::win::mapping_name(ns.c_str(), guid)) == ERROR_SUCCESS);

    wchar_t temp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    const std::filesystem::path dir =
        std::filesystem::path(temp) / (L"isotone-ui-filters-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);

    for (const Case& c : cases) {
        for (ChannelMask mask : {kAllChannels, ChannelMask{0x2}}) {
            CAPTURE(c.name);
            CAPTURE(mask);
            EqState edited;
            edited.bands.push_back(c.band);
            edited.bands[0].channels = mask;
            const EqState sent = state_for_output(edited, 0.0, layout);

            // IsoAPO's region holds float32 parameters.
            {
                DeviceLink link(ns);
                link.set_target(OutputTarget{guid, Backend::native, layout});
                REQUIRE(link.commit(sent) == ERROR_SUCCESS);
                EqState back;
                REQUIRE(link.load_current(&back));
                REQUIRE(back.bands.size() == 1);
                same_response(sent, back, 1e-3);
            }
            // Equalizer APO's Isotone.txt holds 12 significant digits.
            {
                std::filesystem::remove(dir / "Isotone.txt");
                {
                    DeviceLink link(L"Local\\unused.", dir.wstring());
                    link.set_target(OutputTarget{guid, Backend::equalizer_apo, layout});
                    link.commit(sent);
                }   // the destructor flushes the write
                DeviceLink reader(L"Local\\unused.", dir.wstring());
                reader.set_target(OutputTarget{guid, Backend::equalizer_apo, layout});
                EqState back;
                REQUIRE(reader.load_current(&back));
                REQUIRE(back.bands.size() == 1);
                same_response(sent, back, 1e-6);
            }
        }
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("DeviceLink writes an Equalizer APO output's block only when an edit is committed") {
    wchar_t temp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    const std::filesystem::path dir =
        std::filesystem::path(temp) / (L"isotone-ui-compat-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "Isotone.txt";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-00000000beef}";
    {
        DeviceLink link(L"Local\\unused.", dir.wstring());
        link.set_target(OutputTarget{guid, Backend::equalizer_apo, OutputLayout{2, 0x3, 48000}});
        EqState state;
        state.bands.push_back(peaking(1, 1000, -6, 1));
        for (int i = 0; i < 20; ++i) {
            state.bands[0].gain_db = -6.0 + i * 0.1;
            link.apply(state);
        }
        Sleep(300);
        CHECK_FALSE(std::filesystem::exists(file));   // a drag writes nothing

        link.commit(state);
        for (int i = 0; i < 50 && !std::filesystem::exists(file); ++i) Sleep(20);
        REQUIRE(std::filesystem::exists(file));
        CHECK(link.last_compat_error() == ERROR_SUCCESS);
    }
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    CAPTURE(s);
    CHECK(s.find("8f4d2a10-0000-4000-8000-00000000beef") != std::string::npos);
    CHECK(s.find("Fc 1000") != std::string::npos);
    CHECK(s.find("Gain -4.1 dB") != std::string::npos);   // the last value of the drag
    in.close();
    {
        DeviceLink reader(L"Local\\unused.", dir.wstring());
        reader.set_target(OutputTarget{guid, Backend::equalizer_apo, OutputLayout{2, 0x3, 48000}});
        EqState loaded;
        REQUIRE(reader.load_current(&loaded));
        REQUIRE(loaded.bands.size() == 1);
        CHECK(loaded.bands[0].gain_db == doctest::Approx(-4.1));
        OutputTarget elsewhere{L"{8f4d2a10-0000-4000-8000-00000000f00d}", Backend::equalizer_apo, OutputLayout{}};
        reader.set_target(elsewhere);
        CHECK_FALSE(reader.load_current(&loaded));
    }
    std::filesystem::remove_all(dir);
}
