// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The speaker controls without Qt: speakers and groups of a layout, target
// labels, solo, test tone overrides, distance and delay, bass management steps,
// the saved state's speaker part, the graph's channel, and the pink noise.

#include "doctest.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <numeric>

#include "isotone/param_block.h"
#include "persisted_state.h"
#include "pink_noise.h"
#include "speaker_setup.h"
#include "spectrum.h"
#include "test_tone.h"
#include "typed_value.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

constexpr uint32_t k71 = 0x63F, k51 = 0x60F, k21 = 0xB;

Band on(uint32_t id, ChannelMask mask, double fc = 1000, double gain = 3) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = gain;
    b.channels = mask;
    return b;
}

std::string codes(const std::vector<Speaker>& speakers) {
    std::string s;
    for (const Speaker& sp : speakers) s += (s.empty() ? "" : " ") + sp.code;
    return s;
}

}  // namespace

TEST_CASE("the speakers of stereo, 2.1, 5.1 and 7.1, in channel order") {
    CHECK(codes(layout_speakers(2, 0x3)) == "L R");
    CHECK(codes(layout_speakers(3, k21)) == "L R LFE");
    CHECK(codes(layout_speakers(6, k51)) == "L R C LFE SL SR");
    CHECK(codes(layout_speakers(8, k71)) == "L R C LFE RL RR SL SR");
    CHECK(codes(layout_speakers(8, 0)) == "L R C LFE RL RR SL SR");   // the default mask for 8 channels

    const std::vector<Speaker> s = layout_speakers(8, k71);
    const char* names[] = {"Front left", "Front right", "Centre", "Subwoofer", "Rear left", "Rear right", "Side left", "Side right"};
    for (uint32_t c = 0; c < 8; ++c) {
        CAPTURE(c);
        CHECK(s[c].channel == c);
        CHECK(s[c].name == names[c]);
    }
    CHECK(s[3].bit == 0x8);
    CHECK(s[6].bit == 0x200);
    CHECK(layout_speakers(6, k51)[4].name == "Side left");
}

TEST_CASE("group masks follow the layout") {
    const auto find = [](const std::vector<ResolvedGroup>& groups, const std::string& name) -> const ResolvedGroup* {
        for (const ResolvedGroup& g : groups)
            if (g.name == name) return &g;
        return nullptr;
    };
    const std::vector<SpeakerGroup> user = {{"Heights", {"SL", "SR"}}, {"Rears", {"RL", "RR"}}};

    const std::vector<ResolvedGroup> g71 = layout_groups(8, k71, user);
    REQUIRE(g71.size() == 6);
    CHECK(g71[0].name == "All");
    CHECK(g71[0].mask == 0xFF);
    CHECK(g71[1].name == "Front");
    CHECK(g71[1].mask == 0x07);
    CHECK(g71[2].name == "Surround");
    CHECK(g71[2].mask == 0xF0);
    CHECK(g71[3].name == "Sub");
    CHECK(g71[3].mask == 0x08);
    CHECK(g71[3].builtin);
    CHECK(find(g71, "Heights")->mask == 0xC0);
    CHECK_FALSE(find(g71, "Heights")->builtin);
    CHECK(find(g71, "Rears")->mask == 0x30);

    const std::vector<ResolvedGroup> g51 = layout_groups(6, k51, user);
    CHECK(find(g51, "All")->mask == 0x3F);
    CHECK(find(g51, "Front")->mask == 0x07);
    CHECK(find(g51, "Surround")->mask == 0x30);   // SL SR, channels 4 and 5
    CHECK(find(g51, "Sub")->mask == 0x08);
    CHECK(find(g51, "Heights")->mask == 0x30);
    CHECK(find(g51, "Rears") == nullptr);   // 5.1 has neither

    const std::vector<ResolvedGroup> g21 = layout_groups(3, k21, {});
    REQUIRE(g21.size() == 3);
    CHECK(find(g21, "Front")->mask == 0x3);
    CHECK(find(g21, "Sub")->mask == 0x4);
    CHECK(find(g21, "Surround") == nullptr);

    CHECK(codes_mask({"C", "LFE", "XX"}, 8, k71) == 0x0C);
    CHECK(layout_channel_mask(8) == 0xFF);
    CHECK(layout_channel_mask(32) == 0xFFFFFFFFu);
}

TEST_CASE("a band's target label on a surround output") {
    const std::vector<SpeakerGroup> user = {{"Heights", {"SL", "SR"}}};
    CHECK(target_label(kAllChannels, 8, k71, user) == "All");
    CHECK(target_label(0xFF, 8, k71, user) == "All");
    CHECK(target_label(0x1FF, 8, k71, user) == "All");   // bits past the layout are not its speakers
    CHECK(target_label(0x07, 8, k71, user) == "Front");
    CHECK(target_label(0xF0, 8, k71, user) == "Surround");
    CHECK(target_label(0x08, 8, k71, user) == "Sub");     // the group before the speaker's name
    CHECK(target_label(0xC0, 8, k71, user) == "Heights");
    CHECK(target_label(0x04, 8, k71, user) == "Centre");
    CHECK(target_label(0x41, 8, k71, user) == "L SL");
    CHECK(target_label(0x03, 8, k71, user) == "L R");
    CHECK(target_label(0x100, 8, k71, user) == "None");
    CHECK(target_label(0x30, 6, k51, user) == "Surround");   // on 5.1 Surround and Heights are both SL SR
    CHECK(target_label(0x3F, 6, k51, user) == "All");
}

TEST_CASE("solo mutes the other speakers, and not the LFE when the soloed speaker is small") {
    SpeakerSetup setup;
    CHECK(solo_mute_mask(setup, 8, k71, 2) == 0xFB);
    CHECK(solo_mute_mask(setup, 8, k71, 3) == 0xF7);   // the LFE itself
    CHECK(solo_mute_mask(setup, 8, k71, -1) == 0);
    CHECK(solo_mute_mask(setup, 8, k71, 8) == 0);

    setup.small_speakers = 0x04;   // C small, but bass management off: nothing goes to the LFE
    CHECK(solo_mute_mask(setup, 8, k71, 2) == 0xFB);
    setup.bass_management = true;
    CHECK(solo_mute_mask(setup, 8, k71, 2) == 0xF3);
    CHECK(solo_mute_mask(setup, 8, k71, 0) == 0xFE);   // L is not small
    CHECK(solo_mute_mask(setup, 6, k51, 2) == 0x33);

    SpeakerSetup no_lfe;
    no_lfe.bass_management = true;
    no_lfe.small_speakers = 0x1;
    CHECK(solo_mute_mask(no_lfe, 2, 0x3, 0) == 0x2);
}

TEST_CASE("test tones bypass the bands and turn upmix and swaps off; solo adds its mutes") {
    EqState s;
    s.bands = {on(1, 0)};
    s.preamp_db = -4;
    s.speakers.upmix = Upmix::All;
    s.speakers.swap_front_rear = true;
    s.speakers.swap_left_right = true;
    s.speakers.muted = 0x80;
    s.speakers.delay_ms[2] = 1.5;
    s.channel_gain_db[3] = 2;

    EqState live = s;
    apply_live_overrides(LiveOverrides{}, &live);
    CHECK_FALSE(live.bypass);
    CHECK(live.speakers.upmix == Upmix::All);
    CHECK(live.speakers.muted == 0x80);

    live = s;
    apply_live_overrides(LiveOverrides{0x0B, false}, &live);   // solo alone
    CHECK_FALSE(live.bypass);
    CHECK(live.speakers.muted == 0x8B);

    live = s;
    apply_live_overrides(LiveOverrides{0x0B, true}, &live);
    CHECK(live.bypass);
    CHECK(live.speakers.upmix == Upmix::Off);
    CHECK_FALSE(live.speakers.swap_front_rear);
    CHECK_FALSE(live.speakers.swap_left_right);
    CHECK(live.speakers.muted == 0x8B);
    // The speaker's own level and delay still apply to its tone.
    CHECK(live.speakers.delay_ms[2] == 1.5);
    CHECK(live.channel_gain_db[3] == 2);
    CHECK(live.bands.size() == 1);
}

TEST_CASE("distance sets delay from the farthest speaker at 343 m/s") {
    CHECK(delay_ms_for_distance(3.40, 2.20) == doctest::Approx(3.4985).epsilon(1e-4));   // the prototype's rear left
    CHECK(delay_ms_for_distance(3.40, 3.40) == 0.0);
    CHECK(distance_for_delay(3.40, 3.4985) == doctest::Approx(2.20).epsilon(1e-4));

    SpeakerSetup s;
    double farthest = kDefaultFarthestM;
    set_speaker_distance(&s, 8, &farthest, 3, 3.40);   // the LFE, farther than the rest
    CHECK(farthest == doctest::Approx(3.40));
    CHECK(s.delay_ms[3] == 0.0);
    for (uint32_t c : {0u, 1u, 2u, 4u, 7u}) CHECK(s.delay_ms[c] == doctest::Approx(0.40 / 343.0 * 1000.0));

    set_speaker_distance(&s, 8, &farthest, 2, 2.90);
    CHECK(s.delay_ms[2] == doctest::Approx(0.50 / 343.0 * 1000.0));
    CHECK(distance_for_delay(farthest, s.delay_ms[0]) == doctest::Approx(3.0));

    // Bringing the farthest speaker nearer moves the reference to the next farthest.
    set_speaker_distance(&s, 8, &farthest, 3, 1.0);
    CHECK(farthest == doctest::Approx(3.0));
    CHECK(s.delay_ms[0] == doctest::Approx(0.0).scale(1.0));
    CHECK(s.delay_ms[2] == doctest::Approx(0.10 / 343.0 * 1000.0));
    CHECK(s.delay_ms[3] == doctest::Approx(2.0 / 343.0 * 1000.0));

    // Channels past the layout are left alone.
    SpeakerSetup six;
    double f6 = 3.0;
    set_speaker_distance(&six, 6, &f6, 0, 4.0);
    CHECK(six.delay_ms[5] == doctest::Approx(1.0 / 343.0 * 1000.0));
    CHECK(six.delay_ms[6] == 0.0);

    // Out of range values are held, and a non-number changes nothing.
    set_speaker_distance(&six, 6, &f6, 1, -2.0);
    CHECK(six.delay_ms[1] == doctest::Approx(4.0 / 343.0 * 1000.0));
    set_speaker_distance(&six, 6, &f6, 1, NAN);
    CHECK(six.delay_ms[1] == doctest::Approx(4.0 / 343.0 * 1000.0));

    SUBCASE("a typed delay keeps the others' distances") {
        SpeakerSetup d;
        double reference = 3.0;
        set_speaker_delay(&d, &reference, 2, 2.0);
        CHECK(d.delay_ms[2] == 2.0);
        CHECK(reference == 3.0);
        CHECK(distance_for_delay(reference, 2.0) == doctest::Approx(3.0 - 0.686));
        // Nearer than 0 m: the reference grows, and every distance with it.
        set_speaker_delay(&d, &reference, 1, 20.0);
        CHECK(reference == doctest::Approx(6.86));
        CHECK(distance_for_delay(reference, d.delay_ms[1]) == doctest::Approx(0.0).scale(1.0));
        CHECK(distance_for_delay(reference, d.delay_ms[0]) == doctest::Approx(6.86));
        set_speaker_delay(&d, &reference, 1, 5000.0);
        CHECK(d.delay_ms[1] == kMaxSpeakerDelayMs);
        set_speaker_delay(&d, &reference, 1, -3.0);
        CHECK(d.delay_ms[1] == 0.0);
    }
}

TEST_CASE("bass management ranges and 10 Hz steps") {
    CHECK(snap_crossover_hz(80) == 80);
    CHECK(snap_crossover_hz(37) == 40);
    CHECK(snap_crossover_hz(84) == 80);
    CHECK(snap_crossover_hz(86) == 90);
    CHECK(snap_crossover_hz(251) == 250);
    CHECK(snap_crossover_hz(1e6) == 250);
    CHECK(snap_lfe_lowpass_hz(70) == 80);
    CHECK(snap_lfe_lowpass_hz(123) == 120);
    CHECK(snap_lfe_lowpass_hz(300) == 250);
    CHECK(std::isnan(snap_crossover_hz(NAN)));
}

TEST_CASE("the saved state takes the speaker part and keeps its own bands") {
    EqState saved;
    saved.bands = {on(1, 0x30, 100, -6)};   // written for 5.1: SL SR
    saved.preamp_db = -3;
    saved.layout_channels = 6;
    saved.layout_speaker_mask = k51;

    EqState live;
    live.bands = {on(1, 0, 2000, 9), on(2, 0x1)};
    live.preamp_db = -9;
    live.layout_channels = 8;
    live.layout_speaker_mask = k71;
    live.channel_gain_db[2] = 1.5;
    live.speakers.delay_ms[6] = 4.2;
    live.speakers.inverted = 0x8;
    live.speakers.muted = 0x2;
    live.speakers.upmix = Upmix::NoCentre;
    live.speakers.bass_management = true;
    live.speakers.crossover_hz = 100;
    live.speakers.small_speakers = 0xF7;
    live.speakers.lip_sync_ms = 30;

    const EqState merged = saved_with_speakers(&saved, live);
    REQUIRE(merged.bands.size() == 1);
    CHECK(merged.bands[0].gain_db == -6);
    CHECK(merged.bands[0].channels == 0xC0);   // SL SR on 7.1
    CHECK(merged.preamp_db == -3);
    CHECK(merged.layout_channels == 8);
    CHECK(merged.layout_speaker_mask == k71);
    CHECK(merged.channel_gain_db[2] == 1.5);
    CHECK(merged.speakers.delay_ms[6] == 4.2);
    CHECK(merged.speakers.inverted == 0x8);
    CHECK(merged.speakers.muted == 0x2);
    CHECK(merged.speakers.upmix == Upmix::NoCentre);
    CHECK(merged.speakers.small_speakers == 0xF7);
    CHECK(merged.speakers.lip_sync_ms == 30);

    const EqState fresh = saved_with_speakers(nullptr, live);
    CHECK(fresh.bands.empty());
    CHECK(fresh.preamp_db == 0);
    CHECK(fresh.speakers.crossover_hz == 100);

    SUBCASE("written to the file") {
        wchar_t temp[MAX_PATH];
        REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
        const std::filesystem::path dir =
            std::filesystem::path(temp) / (L"isotone-ui-speakers-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(dir);
        const std::wstring path = isotone::win::persisted_state_path(dir.wstring(), L"{8f4d2a10-0000-4000-8000-0000005bea4e}");
        REQUIRE(save_speaker_setup(path, live) == ERROR_SUCCESS);
        ParamBlock block{};
        REQUIRE(isotone::win::read_persisted_state(path, &block) == isotone::win::PersistedRead::Loaded);
        EqState back;
        from_param_block(block, &back);
        CHECK(back.bands.empty());   // no saved state was flat
        CHECK(back.speakers.delay_ms[6] == doctest::Approx(4.2));
        CHECK(back.speakers.upmix == Upmix::NoCentre);

        // A saved preset's bands stay.
        ParamBlock with_bands{};
        to_param_block(saved, &with_bands);
        REQUIRE(isotone::win::write_persisted_state(path, with_bands) == ERROR_SUCCESS);
        live.speakers.lip_sync_ms = 45;
        REQUIRE(save_speaker_setup(path, live) == ERROR_SUCCESS);
        REQUIRE(isotone::win::read_persisted_state(path, &block) == isotone::win::PersistedRead::Loaded);
        from_param_block(block, &back);
        REQUIRE(back.bands.size() == 1);
        CHECK(back.bands[0].gain_db == doctest::Approx(-6));
        CHECK(back.preamp_db == doctest::Approx(-3));
        CHECK(back.speakers.lip_sync_ms == doctest::Approx(45));
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("the graph's channel on a surround output") {
    EqState s;
    s.bands = {on(1, 0x07), on(2, 0x07), on(3, 0x08), on(4, kAllChannels)};
    CHECK(primary_view_channel(s, 8, 0) == 0);      // L has three bands, the LFE two
    CHECK(primary_view_channel(s, 8, 0x08) == 3);   // Sub
    CHECK(primary_view_channel(s, 8, 0xF0) == 4);   // Surround: one band each, the first
    CHECK(primary_view_channel(s, 8, 0x04) == 2);   // one speaker
    s.bands.push_back(on(5, 0x40));
    CHECK(primary_view_channel(s, 8, 0xF0) == 6);

    CHECK(band_view_channel(s.bands[2], 8, 0, 0) == 3);   // a Sub band sits on the LFE's line
    CHECK(band_view_channel(s.bands[0], 8, 0, 0) == 0);
    CHECK(band_view_channel(s.bands[3], 8, 0x08, 3) == 3);
    CHECK(band_view_channel(s.bands[0], 8, 0x08, 3) == 3);   // not in view: on the drawn line

    CHECK(band_in_view(s.bands[2], 8, 0));
    CHECK_FALSE(band_in_view(s.bands[2], 8, 0x07));
    CHECK(band_in_view(s.bands[3], 8, 0x07));
    CHECK(band_in_view(s.bands[0], 8, 0x05));
}

TEST_CASE("pink noise is -30 dBFS RMS and falls 3 dB per octave") {
    constexpr double rate = 48000.0;
    PinkNoise noise(rate);
    const std::vector<float>& x = noise.loop();
    REQUIRE(x.size() >= 4 * 48000);
    REQUIRE((x.size() & (x.size() - 1)) == 0);

    const auto rms_db = [&](size_t from, size_t count) {
        double sum = 0.0;
        for (size_t i = 0; i < count; ++i) {
            const double v = x[(from + i) % x.size()];
            sum += v * v;
        }
        return 10.0 * std::log10(sum / static_cast<double>(count));
    };
    CHECK(rms_db(0, x.size()) == doctest::Approx(-30.0).epsilon(1e-4));
    // Any 3 s, as a meter would read it, wrapping round the loop as it plays.
    for (size_t from = 0; from < x.size(); from += 24000) {
        CAPTURE(from);
        CHECK(std::abs(rms_db(from, 3 * 48000) + 30.0) < 0.2);
    }
    const float peak = *std::max_element(x.begin(), x.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    CHECK(std::abs(peak) < 0.5f);   // no clipping at this level

    // Welch: Hann segments of 16384 with half overlap, power per bin averaged.
    constexpr size_t seg = 16384;
    std::vector<double> window(seg), psd(seg / 2, 0.0);
    for (size_t i = 0; i < seg; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / seg);
    std::vector<std::complex<double>> work(seg);
    size_t segments = 0;
    for (size_t start = 0; start + seg <= x.size(); start += seg / 2, ++segments) {
        for (size_t i = 0; i < seg; ++i) work[i] = x[start + i] * window[i];
        fft(work);
        for (size_t b = 0; b < seg / 2; ++b) psd[b] += std::norm(work[b]);
    }
    // Mean density per octave band from 31.5 Hz to 16 kHz, then a least-squares slope.
    std::vector<double> octave, level;
    for (double centre = 31.5; centre <= 16000.0; centre *= 2.0) {
        const size_t lo = static_cast<size_t>(std::ceil(centre / std::sqrt(2.0) * seg / rate));
        const size_t hi = static_cast<size_t>(std::floor(centre * std::sqrt(2.0) * seg / rate));
        double sum = 0.0;
        for (size_t b = lo; b <= hi; ++b) sum += psd[b];
        octave.push_back(std::log2(centre));
        level.push_back(10.0 * std::log10(sum / static_cast<double>(hi - lo + 1) / static_cast<double>(segments)));
    }
    const double mx = std::accumulate(octave.begin(), octave.end(), 0.0) / static_cast<double>(octave.size());
    const double my = std::accumulate(level.begin(), level.end(), 0.0) / static_cast<double>(level.size());
    double sxy = 0.0, sxx = 0.0;
    for (size_t i = 0; i < octave.size(); ++i) {
        sxy += (octave[i] - mx) * (level[i] - my);
        sxx += (octave[i] - mx) * (octave[i] - mx);
    }
    const double slope = sxy / sxx;
    CAPTURE(slope);
    CHECK(std::abs(slope + 3.01) < 0.25);
    for (size_t i = 0; i < octave.size(); ++i) {
        CAPTURE(std::pow(2.0, octave[i]));
        CHECK(std::abs(level[i] - (my + slope * (octave[i] - mx))) < 1.0);   // no octave off the line
    }

    SUBCASE("at other rates too, and the same loop from the same seed") {
        PinkNoise other(44100.0);
        double sum = 0.0;
        for (float v : other.loop()) sum += static_cast<double>(v) * v;
        CHECK(10.0 * std::log10(sum / static_cast<double>(other.loop().size())) == doctest::Approx(-30.0).epsilon(1e-4));
        PinkNoise again(rate);
        CHECK(again.loop() == x);
    }
}

TEST_CASE("distances and delays are typed in metres and milliseconds") {
    CHECK(*parse_typed_value("3.05 m", TypedUnit::Metres) == doctest::Approx(3.05));
    CHECK(*parse_typed_value("2,5", TypedUnit::Metres) == doctest::Approx(2.5));
    CHECK(*parse_typed_value("1.17 ms", TypedUnit::Milliseconds) == doctest::Approx(1.17));
    CHECK(*parse_typed_value("40MS", TypedUnit::Milliseconds) == doctest::Approx(40));
    CHECK_FALSE(parse_typed_value("3 ms", TypedUnit::Metres).has_value());
    CHECK_FALSE(parse_typed_value("3 m", TypedUnit::Milliseconds).has_value());
    CHECK_FALSE(parse_typed_value("3 dB", TypedUnit::Metres).has_value());
}

TEST_CASE("a test tone on an endpoint that does not exist reports it and plays nothing") {
    TestTone tone;
    std::mutex m;
    std::condition_variable cv;
    HRESULT failed = S_OK;
    tone.start(L"{8f4d2a10-0000-4000-8000-00000000dead}", 0, [&](HRESULT hr, const char*) {
        std::lock_guard<std::mutex> lock(m);
        failed = hr;
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return failed != S_OK; });
    }
    CHECK(failed == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    tone.stop();
    CHECK(tone.frames() == 0);
    CHECK_FALSE(tone.playing());
}
