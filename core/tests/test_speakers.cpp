// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The multichannel stages of the processor: delay, polarity, speaker mute,
// swaps, upmix and bass management.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "isotone/biquad.h"
#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/response.h"
#include "isotone/speakers.h"

using namespace isotone;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kMask51 = 0x60F;   // FL FR FC LFE SL SR
constexpr uint32_t kMask71 = 0x63F;   // FL FR FC LFE BL BR SL SR

// Runs `frames` frames through `p`, `input(channel, frame)` giving each sample,
// and returns the output as [channel][frame].
std::vector<std::vector<float>> run(Processor& p, size_t frames,
                                    const std::function<double(uint32_t, size_t)>& input,
                                    uint32_t block = 256, size_t offset = 0) {
    const uint32_t ch = p.channels();
    std::vector<std::vector<float>> out(ch, std::vector<float>(frames));
    std::vector<std::vector<float>> buf(ch, std::vector<float>(block));
    std::vector<float*> ptr(ch);
    for (size_t pos = 0; pos < frames; pos += block) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, frames - pos));
        for (uint32_t c = 0; c < ch; ++c) {
            for (uint32_t i = 0; i < n; ++i) {
                buf[c][i] = static_cast<float>(input(c, offset + pos + i));
            }
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), n);
        for (uint32_t c = 0; c < ch; ++c) {
            std::copy(buf[c].begin(), buf[c].begin() + n, out[c].begin() + static_cast<long>(pos));
        }
    }
    return out;
}

Processor make(uint32_t channels, uint32_t mask, const EqState& s, uint32_t max_frames = 1024) {
    Processor p;
    p.initialize(kFs, channels, max_frames, 64, mask);
    p.set_target(s);
    p.reset();
    return p;
}

// Amplitude at exactly `bin` cycles over the window: no leakage, no window.
double amplitude(const std::vector<double>& x, uint32_t bin) {
    double re = 0.0, im = 0.0;
    const double n = static_cast<double>(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * bin * static_cast<double>(i) / n;
        re += x[i] * std::cos(w);
        im -= x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

double db(double linear) { return 20.0 * std::log10(linear); }

double worst_step(const std::vector<float>& x) {
    double worst = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - x[i - 1]));
    }
    return worst;
}

// |H|^2 of one second-order Butterworth section at f: a 24 dB/oct
// Linkwitz-Riley filter is two of them in series.
double lr4_db(FilterType type, double fc, double f) {
    Band b;
    b.type = type;
    b.fc = fc;
    b.width = std::sqrt(0.5);
    return 2.0 * magnitude_db(design(b, kFs), f, kFs);
}

}  // namespace

TEST_CASE("speaker positions come from the speaker mask") {
    CHECK(speaker_channel(kMask71, 8, kSpeakerFrontLeft) == 0);
    CHECK(speaker_channel(kMask71, 8, kSpeakerLowFrequency) == 3);
    CHECK(speaker_channel(kMask71, 8, kSpeakerBackRight) == 5);
    CHECK(speaker_channel(kMask71, 8, kSpeakerSideRight) == 7);
    CHECK(speaker_channel(kMask51, 6, kSpeakerSideLeft) == 4);
    CHECK(speaker_channel(0x3, 2, kSpeakerLowFrequency) == -1);
    CHECK(speaker_channel(kMask71, 4, kSpeakerSideLeft) == -1);   // mask wider than the stream
}

TEST_CASE("a delay moves a channel later by whole samples, and lip sync delays every channel") {
    EqState s;
    s.speakers.delay_ms[0] = 10.0;    // 480 samples
    s.speakers.lip_sync_ms = 5.0;     // 240 samples
    Processor p = make(2, 0x3, s);
    const auto out = run(p, 4096, [](uint32_t, size_t i) { return i == 100 ? 1.0 : 0.0; });

    for (uint32_t c = 0; c < 2; ++c) {
        const size_t expected = 100 + 240 + (c == 0 ? 480 : 0);
        const auto peak = std::max_element(out[c].begin(), out[c].end(),
                                           [](float a, float b) { return std::abs(a) < std::abs(b); });
        CAPTURE(c);
        CHECK(static_cast<size_t>(peak - out[c].begin()) == expected);
        CHECK(*peak == doctest::Approx(1.0f));
        double rest = 0.0;
        for (size_t i = 0; i < out[c].size(); ++i) {
            if (i != expected) rest = std::max(rest, std::abs(static_cast<double>(out[c][i])));
        }
        CHECK(rest < 1e-6);
    }
}

TEST_CASE("changing a delay while a sine plays does not click, and the new delay holds") {
    constexpr double kFreq = 1000.0;
    auto sine = [](uint32_t, size_t i) { return std::sin(2.0 * kPi * kFreq * static_cast<double>(i) / kFs); };
    EqState s;
    Processor p = make(2, 0x3, s);
    auto a = run(p, 9600, sine);
    s.speakers.delay_ms[0] = 3.3;    // 158 samples
    p.set_target(s);
    auto b = run(p, 19200, sine, 256, 9600);

    std::vector<float> joined = a[0];
    joined.insert(joined.end(), b[0].begin(), b[0].end());
    const double limit = 2.0 * std::sin(kPi * kFreq / kFs) * 1.10;
    CAPTURE(limit);
    CHECK(worst_step(joined) < limit);

    // Settled: channel 0 is the input 158 samples late.
    double err = 0.0;
    for (size_t i = 9600; i < 19200; ++i) {
        err = std::max(err, std::abs(b[0][i] - sine(0, 9600 + i - 158)));
    }
    CHECK(err < 1e-5);
}

TEST_CASE("polarity inverts a channel without a click") {
    constexpr double kFreq = 1000.0;
    auto sine = [](uint32_t, size_t i) { return std::sin(2.0 * kPi * kFreq * static_cast<double>(i) / kFs); };
    // Switch 12 samples past a zero crossing: a flip exactly at a crossing
    // would be invisible even if it were an instant step.
    constexpr size_t kSwitch = 4812;
    EqState s;
    Processor p = make(2, 0x3, s);
    auto a = run(p, kSwitch, sine);
    s.speakers.inverted = ChannelMask{1} << 0;
    p.set_target(s);
    auto b = run(p, 19200, sine, 256, kSwitch);

    std::vector<float> joined = a[0];
    joined.insert(joined.end(), b[0].begin(), b[0].end());
    CHECK(worst_step(joined) < 2.0 * std::sin(kPi * kFreq / kFs) * 1.10);
    for (size_t i = 14400; i < 19200; i += 97) {
        CHECK(b[0][i] == doctest::Approx(-sine(0, kSwitch + i)).epsilon(1e-3).scale(1.0));
        CHECK(b[1][i] == doctest::Approx(sine(0, kSwitch + i)).epsilon(1e-3).scale(1.0));
    }
}

TEST_CASE("a muted speaker goes silent and the others do not") {
    EqState s;
    s.speakers.muted = ChannelMask{1} << 1;
    Processor p = make(2, 0x3, s);
    const auto out = run(p, 1024, [](uint32_t, size_t) { return 0.5; });
    CHECK(std::abs(out[1][1000]) < 1e-6);
    CHECK(out[0][1000] == doctest::Approx(0.5f));
}

TEST_CASE("muting while audio plays fades to exact zeros, speaker mute and mute alike") {
    // The fade is exponential; without an end it only reaches zero when the
    // sample underflows, which took about 1.5 s in audiodg.
    auto sine = [](uint32_t, size_t i) { return 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs); };
    for (const bool whole : {false, true}) {
        CAPTURE(whole);
        EqState s;
        Processor p = make(2, 0x3, s);
        run(p, 4800, sine);
        if (whole) {
            s.mute = true;
        } else {
            s.speakers.muted = ChannelMask{1} << 1;
        }
        p.set_target(s);
        const auto out = run(p, 48000, sine, 256, 4800);
        // Exact silence well within a second, and the fade itself does not click.
        size_t nonzero = 0;
        for (size_t i = 24000; i < out[1].size(); ++i) nonzero += out[1][i] != 0.0f;
        CHECK(nonzero == 0);
        CHECK(worst_step(out[1]) < 2.0 * std::sin(kPi * 1000.0 / kFs) * 0.5 * 1.10);
        if (!whole) CHECK(std::abs(out[0][40000]) > 0.1);
    }
}

TEST_CASE("swaps move whole speakers: left with right, front with rear") {
    // A different constant on every channel identifies where each one lands.
    auto ident = [](uint32_t c, size_t) { return 0.1 * (c + 1); };
    auto level = [](uint32_t c) { return static_cast<float>(0.1 * (c + 1)); };
    enum { FL, FR, FC, LFE, BL, BR, SL, SR };

    SUBCASE("left and right") {
        EqState s;
        s.speakers.swap_left_right = true;
        Processor p = make(8, kMask71, s);
        const auto out = run(p, 512, ident);
        CHECK(out[FL][500] == doctest::Approx(level(FR)));
        CHECK(out[FR][500] == doctest::Approx(level(FL)));
        CHECK(out[BL][500] == doctest::Approx(level(BR)));
        CHECK(out[SR][500] == doctest::Approx(level(SL)));
        CHECK(out[FC][500] == doctest::Approx(level(FC)));
        CHECK(out[LFE][500] == doctest::Approx(level(LFE)));
    }
    SUBCASE("front and rear") {
        EqState s;
        s.speakers.swap_front_rear = true;
        Processor p = make(8, kMask71, s);
        const auto out = run(p, 512, ident);
        CHECK(out[FL][500] == doctest::Approx(level(BL)));
        CHECK(out[BR][500] == doctest::Approx(level(FR)));
        CHECK(out[SL][500] == doctest::Approx(level(SL)));
    }
    SUBCASE("front and rear on a 5.1 with side surrounds") {
        // Windows' default 6-channel layout: FL FR FC LFE SL SR.
        enum { L5, R5, C5, LFE5, SL5, SR5 };
        EqState s;
        s.speakers.swap_front_rear = true;
        Processor p = make(6, 0x60F, s);
        const auto out = run(p, 512, ident);
        CHECK(out[L5][500] == doctest::Approx(level(SL5)));
        CHECK(out[SR5][500] == doctest::Approx(level(R5)));
        CHECK(out[C5][500] == doctest::Approx(level(C5)));
        CHECK(out[LFE5][500] == doctest::Approx(level(LFE5)));
    }
    SUBCASE("both") {
        EqState s;
        s.speakers.swap_left_right = true;
        s.speakers.swap_front_rear = true;
        Processor p = make(8, kMask71, s);
        const auto out = run(p, 512, ident);
        CHECK(out[FL][500] == doctest::Approx(level(BR)));
        CHECK(out[BR][500] == doctest::Approx(level(FL)));
    }
}

TEST_CASE("swapping speakers while audio plays crossfades instead of clicking") {
    constexpr double kFreq = 1000.0;
    // Left and right a quarter cycle apart, so a hard swap is a large step.
    auto tones = [](uint32_t c, size_t i) {
        const double w = 2.0 * kPi * kFreq * static_cast<double>(i) / kFs;
        return c == 0 ? std::sin(w) : std::cos(w);
    };
    constexpr size_t kSwitch = 4812;
    EqState s;
    Processor p = make(2, 0x3, s);
    auto a = run(p, kSwitch, tones);
    s.speakers.swap_left_right = true;
    p.set_target(s);
    auto b = run(p, 19200, tones, 256, kSwitch);

    std::vector<float> joined = a[0];
    joined.insert(joined.end(), b[0].begin(), b[0].end());
    CHECK(worst_step(joined) < 2.0 * std::sin(kPi * kFreq / kFs) * 1.10);
    CHECK(b[0][19000] == doctest::Approx(tones(1, kSwitch + 19000)).epsilon(1e-4).scale(1.0));
}

TEST_CASE("upmix feeds the front pair into the other speakers") {
    enum { FL, FR, FC, LFE, BL, BR, SL, SR };
    auto fronts = [](uint32_t c, size_t) { return c == FL ? 1.0 : c == FR ? 0.5 : 0.0; };
    const double g = std::sqrt(0.5);

    EqState s;
    s.speakers.upmix = Upmix::All;
    Processor p = make(8, kMask71, s);
    auto out = run(p, 512, fronts);
    CHECK(out[FL][500] == doctest::Approx(1.0f));
    CHECK(out[FR][500] == doctest::Approx(0.5f));
    CHECK(out[FC][500] == doctest::Approx(0.75f));
    CHECK(out[SL][500] == doctest::Approx(g));
    CHECK(out[BL][500] == doctest::Approx(g));
    CHECK(out[SR][500] == doctest::Approx(0.5 * g));
    CHECK(out[BR][500] == doctest::Approx(0.5 * g));
    CHECK(std::abs(out[LFE][500]) < 1e-6);

    s.speakers.upmix = Upmix::NoCentre;
    p = make(8, kMask71, s);
    out = run(p, 512, fronts);
    CHECK(std::abs(out[FC][500]) < 1e-6);
    CHECK(out[SL][500] == doctest::Approx(g));
}

TEST_CASE("bass management: a small speaker and the sub sum flat through the crossover") {
    enum { FL, FR, FC, LFE, SL, SR };
    constexpr size_t kN = 16384;
    EqState s;
    s.speakers.bass_management = true;
    s.speakers.crossover_hz = 80.0;
    s.speakers.small_speakers = ChannelMask{1} << FL;

    for (uint32_t bin : {14u, 27u, 55u, 341u}) {
        const double f = bin * kFs / kN;
        Processor p = make(6, kMask51, s);
        auto tone = [&](uint32_t c, size_t i) { return c == FL ? std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs) : 0.0; };
        run(p, 32768, tone);
        const auto out = run(p, kN, tone, 256, 32768);

        std::vector<double> main(kN), sub(kN), sum(kN);
        for (size_t i = 0; i < kN; ++i) {
            main[i] = out[FL][i];
            sub[i] = out[LFE][i];
            sum[i] = main[i] + sub[i];
        }
        CAPTURE(f);
        CHECK(std::abs(db(amplitude(sum, bin))) < 0.02);
        CHECK(std::abs(db(amplitude(main, bin)) - lr4_db(FilterType::HighPass, 80.0, f)) < 0.02);
        CHECK(std::abs(db(amplitude(sub, bin)) - lr4_db(FilterType::LowPass, 80.0, f)) < 0.02);
        std::vector<double> other(kN);
        for (size_t i = 0; i < kN; ++i) other[i] = out[FR][i];
        CHECK(amplitude(other, bin) < 1e-6);   // a speaker that is not small gets none of it
    }
}

TEST_CASE("the peak auto preamp negates includes routing and bass management") {
    // Upmix with every speaker small: the sub carries the bass of all seven
    // speakers at once, far above any input's level. The worst case is every
    // path into one output arriving in phase. The seven redirected speakers
    // share the crossover's phase; the LFE channel's own content goes through
    // its own low-pass, so its input's phase is searched to line it up.
    // No bands: their centres are always evaluated, which would move the peak
    // away from the one frequency tested.
    EqState s;
    s.speakers.upmix = Upmix::All;
    s.speakers.bass_management = true;
    s.speakers.small_speakers = 0xF7;
    s.channel_gain_db[6] = 2.0;
    for (const double f : {40.0, 100.0, 1000.0}) {
        CAPTURE(f);
        const double bound = composite_peak_db(s, 8, kMask71, &f, 1, kFs);
        double loudest = 0.0;
        for (int step = 0; step < 36; ++step) {
            const double lfe_phase = 2.0 * kPi * step / 36.0;
            Processor p = make(8, kMask71, s, 1024);
            const auto out = run(p, 48000, [f, lfe_phase](uint32_t c, size_t i) {
                return std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs + (c == 3 ? lfe_phase : 0.0));
            });
            for (uint32_t c = 0; c < 8; ++c) {
                const std::vector<double> tail(out[c].begin() + 24000, out[c].end());
                loudest = std::max(loudest, amplitude(tail, static_cast<uint32_t>(f / 2.0)));
            }
        }
        CHECK(db(loudest) <= bound + 0.001);
        CHECK(bound - db(loudest) < 0.03);
    }
    // Far above 0 dB: this is what the engine's limiter caught at 7.1.
    const double f40 = 40.0;
    CHECK(composite_peak_db(s, 8, kMask71, &f40, 1, kFs) > 12.0);
    // A muted output plays nothing, so it needs no headroom.
    s.speakers.muted = ChannelMask{1} << 3;
    CHECK(composite_peak_db(s, 8, kMask71, &f40, 1, kFs) < 0.0);
}

TEST_CASE("the LFE low-pass filters the LFE channel's own content") {
    enum { FL, FR, FC, LFE, SL, SR };
    constexpr size_t kN = 16384;
    EqState s;
    s.speakers.bass_management = true;
    s.speakers.lfe_lowpass_hz = 120.0;

    for (uint32_t bin : {14u, 170u}) {
        const double f = bin * kFs / kN;
        Processor p = make(6, kMask51, s);
        auto tone = [&](uint32_t c, size_t i) { return c == LFE ? std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs) : 0.0; };
        run(p, 32768, tone);
        const auto out = run(p, kN, tone, 256, 32768);
        std::vector<double> sub(out[LFE].begin(), out[LFE].end());
        CAPTURE(f);
        CHECK(std::abs(db(amplitude(sub, bin)) - lr4_db(FilterType::LowPass, 120.0, f)) < 0.02);
    }
}

TEST_CASE("bass management does nothing on a stream with no LFE channel") {
    EqState s;
    s.speakers.bass_management = true;
    s.speakers.small_speakers = 0x3;
    Processor p = make(2, 0x3, s);
    auto tone = [](uint32_t, size_t i) { return std::sin(2.0 * kPi * 40.0 * static_cast<double>(i) / kFs); };
    const auto out = run(p, 4800, tone);
    for (size_t i = 0; i < 4800; i += 31) {
        CHECK(out[0][i] == doctest::Approx(tone(0, i)).epsilon(1e-5).scale(1.0));
    }
}

TEST_CASE("turning bass management on or off while bass plays does not click") {
    enum { FL, FR, FC, LFE, SL, SR };
    constexpr double kFreq = 100.0;
    auto tone = [](uint32_t c, size_t i) { return c == FL ? std::sin(2.0 * kPi * kFreq * static_cast<double>(i) / kFs) : 0.0; };
    EqState s;
    s.speakers.small_speakers = ChannelMask{1} << FL;
    Processor p = make(6, kMask51, s);
    // Switch points deliberately off a zero crossing.
    constexpr size_t kOn = 9637, kOff = kOn + 24000;
    auto a = run(p, kOn, tone);
    s.speakers.bass_management = true;
    p.set_target(s);
    auto b = run(p, kOff - kOn, tone, 256, kOn);
    s.speakers.bass_management = false;
    p.set_target(s);
    auto c2 = run(p, 19200, tone, 256, kOff);

    // The sine itself moves at most 0.0131 per sample at 100 Hz. A click is a
    // step on the order of the signal, so a bound a little above the sine's
    // own slope separates the two by more than an order of magnitude.
    const double limit = 2.0 * std::sin(kPi * kFreq / kFs) * 1.25;
    for (uint32_t c : {static_cast<uint32_t>(FL), static_cast<uint32_t>(LFE)}) {
        std::vector<float> joined = a[c];
        joined.insert(joined.end(), b[c].begin(), b[c].end());
        joined.insert(joined.end(), c2[c].begin(), c2[c].end());
        CAPTURE(c);
        CHECK(worst_step(joined) < limit);
    }
    // And each switch took effect: the sub carried part of the tone, then none.
    std::vector<double> on(b[LFE].end() - 4800, b[LFE].end());
    CHECK(amplitude(on, 10) > 0.1);
    std::vector<double> off(c2[LFE].end() - 4800, c2[LFE].end());
    CHECK(amplitude(off, 10) < 1e-4);
}

TEST_CASE("hostile speaker values cannot poison the processor") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EqState s;
    s.speakers.delay_ms[0] = nan;
    s.speakers.delay_ms[1] = 1e9;
    s.speakers.lip_sync_ms = -inf;
    s.speakers.bass_management = true;
    s.speakers.crossover_hz = inf;
    s.speakers.lfe_lowpass_hz = nan;
    s.speakers.small_speakers = 0xFFFFFFFFu;
    s.speakers.inverted = 0xFFFFFFFFu;
    Processor p = make(8, kMask71, s);
    const auto out = run(p, 96000, [](uint32_t c, size_t i) { return std::sin(0.01 * static_cast<double>(i) * (c + 1)); });
    bool finite = true;
    double loudest = 0.0;
    for (const auto& ch : out) {
        for (float v : ch) {
            finite &= std::isfinite(v);
            loudest = std::max(loudest, std::abs(static_cast<double>(v)));
        }
    }
    CHECK(finite);
    CHECK(loudest > 0.1);   // finite silence would also pass the check above
}

TEST_CASE("a delay rounds to the nearest whole sample") {
    // 0.7 ms is 33.6 samples at 48 kHz and 30.87 at 44.1 kHz: the rounding
    // Equalizer APO's DelayFilter does, not truncation.
    for (const auto& [fs, expected] : {std::pair{48000.0, size_t{34}}, std::pair{44100.0, size_t{31}}}) {
        EqState s;
        s.speakers.delay_ms[0] = 0.7;
        Processor p;
        p.initialize(fs, 2, 1024, 64, 0x3);
        p.set_target(s);
        p.reset();
        const auto out = run(p, 256, [](uint32_t, size_t i) { return i == 10 ? 1.0 : 0.0; });
        CAPTURE(fs);
        CHECK(out[0][10 + expected] == doctest::Approx(1.0f));
    }
}

TEST_CASE("lip sync delays channels past the eighth too") {
    EqState s;
    s.speakers.lip_sync_ms = 1.0;   // 48 samples
    Processor p = make(12, 0x2D63F, s);
    const auto out = run(p, 256, [](uint32_t, size_t i) { return i == 5 ? 1.0 : 0.0; });
    for (uint32_t c = 0; c < 12; ++c) {
        CAPTURE(c);
        CHECK(out[c][53] == doctest::Approx(1.0f));
        CHECK(out[c][5] == 0.0f);
    }
}

TEST_CASE("a delay changed again while its crossfade runs ends on the last value, without a click") {
    constexpr double kFreq = 50.0;
    auto sine = [](uint32_t, size_t i) { return std::sin(2.0 * kPi * kFreq * static_cast<double>(i) / kFs); };
    EqState s;
    Processor p = make(2, 0x3, s);
    auto a = run(p, 4800, sine);
    s.speakers.delay_ms[0] = 2.0;
    p.set_target(s);
    auto b = run(p, 96, sine, 48, 4800);          // 2 ms into the 10 ms delay fade
    s.speakers.delay_ms[0] = 5.0;                  // 240 samples
    p.set_target(s);
    auto c = run(p, 19104, sine, 48, 4896);
    std::vector<float> joined = a[0];
    joined.insert(joined.end(), b[0].begin(), b[0].end());
    joined.insert(joined.end(), c[0].begin(), c[0].end());
    CHECK(worst_step(joined) < 2.0 * std::sin(kPi * kFreq / kFs) * 1.10);
    double err = 0.0;
    for (size_t i = 9600; i < c[0].size(); ++i) err = std::max(err, std::abs(c[0][i] - sine(0, 4896 + i - 240)));
    CHECK(err < 1e-5);
}

TEST_CASE("the speaker setup survives a trip through the param block") {
    EqState s;
    s.speakers.delay_ms[2] = 4.5;
    s.speakers.delay_ms[7] = 12.25;
    s.speakers.inverted = 0x8;
    s.speakers.muted = 0x40;
    s.speakers.lip_sync_ms = 120.0;
    s.speakers.swap_left_right = true;
    s.speakers.swap_front_rear = true;
    s.speakers.upmix = Upmix::NoCentre;
    s.speakers.bass_management = true;
    s.speakers.crossover_hz = 90.0;
    s.speakers.small_speakers = 0x33;
    s.speakers.lfe_lowpass_hz = 110.0;

    ParamBlock block{};
    init_param_block(&block);
    to_param_block(s, &block);
    EqState back;
    from_param_block(block, &back);

    const SpeakerSetup& a = s.speakers;
    const SpeakerSetup& b = back.speakers;
    for (uint32_t c = 0; c < kMaxChannels; ++c) CHECK(b.delay_ms[c] == doctest::Approx(a.delay_ms[c]));
    CHECK(b.inverted == a.inverted);
    CHECK(b.muted == a.muted);
    CHECK(b.lip_sync_ms == doctest::Approx(a.lip_sync_ms));
    CHECK(b.swap_left_right);
    CHECK(b.swap_front_rear);
    CHECK(b.upmix == Upmix::NoCentre);
    CHECK(b.bass_management);
    CHECK(b.crossover_hz == doctest::Approx(a.crossover_hz));
    CHECK(b.small_speakers == a.small_speakers);
    CHECK(b.lfe_lowpass_hz == doctest::Approx(a.lfe_lowpass_hz));

    block.speakers.upmix = 9999;
    from_param_block(block, &back);
    CHECK(static_cast<uint32_t>(back.speakers.upmix) <= 2);
}

TEST_CASE("the speaker setup text round-trips and rejects what it cannot read") {
    SpeakerSetup a;
    a.delay_ms[2] = 1.25;
    a.delay_ms[7] = 30;
    a.lip_sync_ms = 120;
    a.inverted = 0x8;
    a.muted = 0x40;
    a.swap_left_right = true;
    a.upmix = Upmix::NoCentre;
    a.bass_management = true;
    a.crossover_hz = 90;
    a.small_speakers = 0x37;
    a.lfe_lowpass_hz = 110;
    SpeakerSetup b;
    std::string error;
    REQUIRE(parse_speaker_setup(format_speaker_setup(a), &b, &error));
    CHECK(format_speaker_setup(b) == format_speaker_setup(a));

    SpeakerSetup c;
    CHECK(parse_speaker_setup("", &c, &error));
    CHECK(format_speaker_setup(c) == format_speaker_setup(SpeakerSetup{}));
    CHECK(parse_speaker_setup("swap_left_right=1 crossover_hz=70", &c, &error));
    CHECK(c.swap_left_right);
    CHECK(c.crossover_hz == 70.0);

    for (const char* bad : {"swap=1", "upmix=sideways", "crossover_hz=nan", "delay_ms=1,2,3,4,5,6,7,8,9",
                            "inverted=0x1ffffffff", "bass_management=yes", "lip_sync_ms"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_speaker_setup(bad, &c, &error));
        CHECK(!error.empty());
    }
}
