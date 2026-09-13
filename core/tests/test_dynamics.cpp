// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// How parameters move over time, measured on the output. The click tests in
// test_smoothing.cpp bound the worst sample-to-sample step of a 1 kHz tone, which
// is dominated by the sine's own slope: a change made instant, or a crossfade
// shortened to one control block, passes them (found by mutation in the
// 2026-09-13 review). These tests measure the level at a known time after a
// change, so a smoother or crossfade running at the wrong speed fails, and they
// make each change at a peak of a 50 Hz tone, where a step cannot hide.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <vector>

#include "isotone/biquad.h"
#include "isotone/processor.h"

using namespace isotone;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kE  = 2.71828182845904523536;

using Signal = std::vector<std::vector<float>>;   // [channel][frame]

struct Setup {
    double   fs       = 48000.0;
    uint32_t channels = 2;
    uint32_t mask     = 0;
    uint32_t block    = 48;
};

// Renders `frames` frames. `input(channel, frame)` gives each input sample;
// `mutate(frame, state)` runs before each block and returns true when it changed
// the state, which is then applied with set_target.
Signal render(EqState s, const Setup& setup, size_t frames,
              const std::function<double(uint32_t, size_t)>& input,
              const std::function<bool(size_t, EqState&)>& mutate = nullptr) {
    Processor p;
    p.initialize(setup.fs, setup.channels, setup.block, 64, setup.mask);
    p.set_target(s);
    p.reset();
    Signal out(setup.channels, std::vector<float>(frames));
    std::vector<std::vector<float>> buf(setup.channels, std::vector<float>(setup.block));
    std::vector<float*> ptr(setup.channels);
    for (size_t pos = 0; pos < frames; pos += setup.block) {
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(setup.block, frames - pos));
        if (mutate && mutate(pos, s)) {
            p.set_target(s);
        }
        for (uint32_t c = 0; c < setup.channels; ++c) {
            for (uint32_t i = 0; i < n; ++i) buf[c][i] = static_cast<float>(input(c, pos + i));
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), n);
        for (uint32_t c = 0; c < setup.channels; ++c) {
            std::copy(buf[c].begin(), buf[c].begin() + n, out[c].begin() + static_cast<long>(pos));
        }
    }
    return out;
}

std::function<double(uint32_t, size_t)> sine(double freq, double fs = 48000.0, double amp = 1.0) {
    return [=](uint32_t, size_t i) { return amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / fs); };
}

// Complex amplitude of `freq` over `cycles` whole cycles centred on `centre`,
// relative to the input sine's phase (sin at frame 0).
std::complex<double> phasor(const std::vector<float>& x, size_t centre, double freq, double fs, int cycles = 1) {
    const size_t n = static_cast<size_t>(std::llround(fs / freq * cycles));
    const size_t start = centre - n / 2;
    std::complex<double> acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 2.0 * kPi * freq * static_cast<double>(start + i) / fs;
        acc += static_cast<double>(x[start + i]) * std::complex<double>(std::sin(w), std::cos(w));
    }
    return acc * (2.0 / static_cast<double>(n));
}

double level_db(const std::vector<float>& x, size_t centre, double freq, double fs, int cycles = 1) {
    return 20.0 * std::log10(std::abs(phasor(x, centre, freq, fs, cycles)));
}

// The in-phase part: signed, so a polarity flip reads negative.
double in_phase(const std::vector<float>& x, size_t centre, double freq, double fs) {
    return phasor(x, centre, freq, fs).real();
}

double worst_step(const std::vector<float>& x) {
    double worst = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    }
    return worst;
}

Band peaking(double fc, double gain_db, double q, uint32_t id = 0) {
    Band b;
    b.id = id;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    return b;
}

// |H| in dB of a 24 dB/oct Linkwitz-Riley filter: two Butterworth sections.
double lr4_db(FilterType type, double fc, double f, double fs) {
    return 2.0 * magnitude_db(butterworth2(type, fc, fs), f, fs);
}

constexpr size_t kChange = 9600;   // 200 ms in at 48 kHz, a multiple of every block used
constexpr size_t kTau    = 960;    // kSmoothingTauSeconds at 48 kHz
constexpr size_t kHalfFade = 240;  // half of kCrossfadeSeconds at 48 kHz

}  // namespace

TEST_CASE("smoothed parameters move with the 20 ms time constant") {
    const Setup st;
    const double one_tau = -12.0 * (1.0 - 1.0 / kE);   // dB after one time constant

    SUBCASE("band gain") {
        EqState s;
        s.bands.push_back(peaking(1000.0, 0.0, 1.0));
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.bands[0].gain_db = -12.0;
            return true;
        });
        CHECK(std::abs(level_db(out[0], kChange + kTau, 1000.0, st.fs) - one_tau) < 1.0);
        CHECK(std::abs(level_db(out[0], kChange + 14400, 1000.0, st.fs) - -12.0) < 0.05);
    }
    SUBCASE("preamp") {
        EqState s;
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.preamp_db = -12.0;
            return true;
        });
        CHECK(std::abs(level_db(out[0], kChange + kTau, 1000.0, st.fs) - one_tau) < 1.0);
        CHECK(std::abs(level_db(out[0], kChange + 14400, 1000.0, st.fs) - -12.0) < 0.05);
    }
    SUBCASE("channel trim") {
        EqState s;
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.channel_gain_db[0] = -12.0;
            return true;
        });
        CHECK(std::abs(level_db(out[0], kChange + kTau, 1000.0, st.fs) - one_tau) < 1.0);
        CHECK(std::abs(level_db(out[1], kChange + kTau, 1000.0, st.fs)) < 0.01);
    }
    SUBCASE("mute") {
        EqState s;
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.mute = true;
            return true;
        });
        CHECK(std::abs(in_phase(out[0], kChange + kTau, 1000.0, st.fs) - 1.0 / kE) < 0.06);
    }
    SUBCASE("polarity") {
        EqState s;
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.speakers.inverted = 1;
            return true;
        });
        CHECK(std::abs(in_phase(out[0], kChange + kTau, 1000.0, st.fs) - (-1.0 + 2.0 / kE)) < 0.08);
        CHECK(std::abs(in_phase(out[0], kChange + 14400, 1000.0, st.fs) - -1.0) < 0.01);
    }
    SUBCASE("Q, measured off the band centre where it changes the level") {
        EqState s;
        s.bands.push_back(peaking(1500.0, 9.0, 0.5));
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.bands[0].width = 8.0;
            return true;
        });
        const double final_db = level_db(out[0], kChange + 14400, 1000.0, st.fs);
        CHECK(std::abs(level_db(out[0], kChange + kTau, 1000.0, st.fs) - final_db) > 0.3);
    }
    SUBCASE("frequency sweeps through the tone instead of jumping over it") {
        EqState s;
        s.bands.push_back(peaking(500.0, 12.0, 2.0));
        const Signal out = render(s, st, 48000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.bands[0].fc = 2000.0;
            return true;
        });
        const double before = level_db(out[0], kChange - 480, 1000.0, st.fs);
        const double after  = level_db(out[0], kChange + 14400, 1000.0, st.fs);
        double highest = -1e9;
        for (size_t c = kChange; c < kChange + 4800; c += 48) highest = std::max(highest, level_db(out[0], c, 1000.0, st.fs));
        CHECK(highest > std::max(before, after) + 3.0);
    }
}

TEST_CASE("crossfades take 10 ms") {
    const Setup st;
    SUBCASE("disabling a band") {
        EqState s;
        s.bands.push_back(peaking(1000.0, 12.0, 1.0));
        const Signal out = render(s, st, 24000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.bands[0].enabled = false;
            return true;
        });
        const double boosted = std::pow(10.0, 12.0 / 20.0);
        CHECK(std::abs(std::abs(phasor(out[0], kChange + kHalfFade, 1000.0, st.fs)) - (boosted + 1.0) / 2.0) < 0.15);
        CHECK(std::abs(std::abs(phasor(out[0], kChange + 2400, 1000.0, st.fs)) - 1.0) < 0.001);
    }
    SUBCASE("bypass") {
        EqState s;
        s.bands.push_back(peaking(1000.0, -12.0, 1.0));
        const Signal out = render(s, st, 24000, sine(1000.0), [](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            e.bypass = true;
            return true;
        });
        const double cut = std::pow(10.0, -12.0 / 20.0);
        CHECK(std::abs(std::abs(phasor(out[0], kChange + kHalfFade, 1000.0, st.fs)) - (cut + 1.0) / 2.0) < 0.05);
    }
}

TEST_CASE("a crossfade runs at the same speed whatever the host's buffer size") {
    // 44.1 kHz: the control block is 29 frames, so a 441-frame buffer ends in a
    // partial block every time.
    constexpr double fs = 44100.0;
    auto disable_at = [](size_t at) {
        return [at](size_t pos, EqState& e) {
            if (pos != at) return false;
            e.bands[0].enabled = false;
            return true;
        };
    };
    EqState s;
    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    Setup one;   one.fs = fs; one.block = 1;
    Setup big;   big.fs = fs; big.block = 441;
    const Signal a = render(s, one, 13230, sine(1000.0, fs), disable_at(4410));
    const Signal b = render(s, big, 13230, sine(1000.0, fs), disable_at(4410));
    double worst = 0.0;
    for (size_t i = 0; i < a[0].size(); ++i) worst = std::max(worst, std::abs(double(a[0][i]) - b[0][i]));
    CHECK(worst < 1e-5);
}

TEST_CASE("no change clicks when it lands on a peak of a low tone") {
    // 50 Hz, changed exactly a quarter cycle after a zero crossing: the tone's
    // own slope is near zero there, so any step shows. The bound is the largest
    // step a clean 50 Hz sine of the loudest amplitude involved can take.
    constexpr double kLow = 50.0;
    constexpr size_t kPeak = 5040;   // 5.25 cycles; a multiple of the 48-frame block
    const Setup st;
    auto limit = [](double amplitude) { return amplitude * 2.0 * std::sin(kPi * kLow / 48000.0) * 1.10; };

    struct Case {
        const char* name;
        std::function<void(EqState&)> before, change;
        double loudest;
    };
    const Case cases[] = {
        {"band gain", [](EqState& e) { e.bands.push_back(peaking(kLow, 0.0, 1.0)); },
         [](EqState& e) { e.bands[0].gain_db = -12.0; }, 1.0},
        {"preamp", [](EqState&) {}, [](EqState& e) { e.preamp_db = -12.0; }, 1.0},
        {"trim", [](EqState&) {}, [](EqState& e) { e.channel_gain_db[0] = -12.0; }, 1.0},
        {"mute", [](EqState&) {}, [](EqState& e) { e.mute = true; }, 1.0},
        {"speaker mute", [](EqState&) {}, [](EqState& e) { e.speakers.muted = 1; }, 1.0},
        {"polarity", [](EqState&) {}, [](EqState& e) { e.speakers.inverted = 1; }, 1.0},
        {"bypass", [](EqState& e) { e.bands.push_back(peaking(kLow, -12.0, 1.0)); },
         [](EqState& e) { e.bypass = true; }, 1.0},
        {"band enable", [](EqState& e) { e.bands.push_back(peaking(kLow, 12.0, 1.0)); },
         [](EqState& e) { e.bands[0].enabled = false; }, std::pow(10.0, 12.0 / 20.0)},
        {"band type", [](EqState& e) { e.bands.push_back(peaking(kLow, 12.0, 1.0)); },
         [](EqState& e) { e.bands[0].type = FilterType::LowShelf; e.bands[0].width = 0.7; }, std::pow(10.0, 12.5 / 20.0)},
        {"band channels", [](EqState& e) { e.bands.push_back(peaking(kLow, -12.0, 1.0)); },
         [](EqState& e) { e.bands[0].channels = 2; }, 1.0},
        {"preset", [](EqState& e) { e.bands.push_back(peaking(kLow, 9.0, 2.0, 1)); e.bands.push_back(peaking(200.0, -6.0, 1.0, 2)); },
         [](EqState& e) { e.bands.clear(); e.bands.push_back(peaking(40.0, -9.0, 0.7, 3)); e.preamp_db = -4.0; },
         std::pow(10.0, 9.0 / 20.0)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        EqState s;
        c.before(s);
        const Signal out = render(s, st, 14400, sine(kLow), [&](size_t pos, EqState& e) {
            if (pos != kPeak) return false;
            c.change(e);
            return true;
        });
        CHECK(worst_step(out[0]) < limit(c.loudest));
    }
}

TEST_CASE("the crossover and LFE low-pass follow a change while audio plays") {
    enum { FL, FR, FC, LFE, SL, SR };
    Setup st;
    st.channels = 6;
    st.mask = 0x60F;
    constexpr double f = 34.0 * 48000.0 / 16384.0;   // 99.6 Hz

    SUBCASE("crossover") {
        EqState s;
        s.speakers.bass_management = true;
        s.speakers.small_speakers = ChannelMask{1} << FL;
        s.speakers.crossover_hz = 80.0;
        const Signal out = render(s, st, 64384, [](uint32_t c, size_t i) { return c == FL ? std::sin(2.0 * kPi * f * i / 48000.0) : 0.0; },
                                  [](size_t pos, EqState& e) {
                                      if (pos != 0) return false;
                                      e.speakers.crossover_hz = 200.0;
                                      return true;
                                  });
        CHECK(std::abs(level_db(out[FL], 56192, f, 48000.0, 20) - lr4_db(FilterType::HighPass, 200.0, f, 48000.0)) < 0.05);
    }
    SUBCASE("LFE low-pass") {
        EqState s;
        s.speakers.bass_management = true;
        s.speakers.lfe_lowpass_hz = 120.0;
        const Signal out = render(s, st, 64384, [](uint32_t c, size_t i) { return c == LFE ? std::sin(2.0 * kPi * f * i / 48000.0) : 0.0; },
                                  [](size_t pos, EqState& e) {
                                      if (pos != 0) return false;
                                      e.speakers.lfe_lowpass_hz = 80.0;
                                      return true;
                                  });
        CHECK(std::abs(level_db(out[LFE], 56192, f, 48000.0, 20) - lr4_db(FilterType::LowPass, 80.0, f, 48000.0)) < 0.05);
    }
}

TEST_CASE("a NaN on a bass-managed speaker does not silence it") {
    enum { FL, FR, FC, LFE, SL, SR };
    Setup st;
    st.channels = 6;
    st.mask = 0x60F;
    EqState s;
    s.speakers.bass_management = true;
    s.speakers.small_speakers = ChannelMask{1} << FL;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Signal out = render(s, st, 24000, [nan](uint32_t c, size_t i) {
        if (c != FL) return 0.0;
        return i == 4800 ? nan : std::sin(2.0 * kPi * 1000.0 * i / 48000.0);
    });
    double peak = 0.0;
    for (size_t i = 14400; i < out[FL].size(); ++i) peak = std::max(peak, std::abs(double(out[FL][i])));
    CHECK(peak > 0.9);
    bool finite = true;
    for (const auto& ch : out) for (float v : ch) finite &= std::isfinite(v);
    CHECK(finite);
}

TEST_CASE("deleting, inserting or reordering bands leaves the other bands alone") {
    // Bands are matched to filters by id, so an edit to the list touches only
    // the band it is about. Matched by position, every later band would take
    // over its neighbour's filter and sweep from its values (review 2026-09-13).
    const Setup st;
    auto five = [] {
        EqState s;
        s.bands.push_back(peaking(100.0, -9.0, 1.0, 1));
        s.bands.push_back(peaking(300.0, 9.0, 1.0, 2));
        s.bands.push_back(peaking(1000.0, 9.0, 4.0, 3));
        s.bands.push_back(peaking(3000.0, -9.0, 1.0, 4));
        s.bands.push_back(peaking(10000.0, 9.0, 1.0, 5));
        return s;
    };
    // The level at `freq` every 1 ms from 10 ms before the edit to 200 ms after
    // stays between the settled levels before and after, give or take `slack`.
    auto stays_between = [&](const std::function<void(EqState&)>& edit, double freq, double slack) {
        const Signal out = render(five(), st, 48000, sine(freq), [&](size_t pos, EqState& e) {
            if (pos != kChange) return false;
            edit(e);
            return true;
        });
        const double before = level_db(out[0], kChange - 480, freq, st.fs, 5);
        const double after  = level_db(out[0], kChange + 14400, freq, st.fs, 5);
        double lo = 1e9, hi = -1e9;
        for (size_t c = kChange - 480; c < kChange + 9600; c += 48) {
            const double l = level_db(out[0], c, freq, st.fs, 5);
            lo = std::min(lo, l);
            hi = std::max(hi, l);
        }
        CAPTURE(before);
        CAPTURE(after);
        CHECK(lo > std::min(before, after) - slack);
        CHECK(hi < std::max(before, after) + slack);
    };
    SUBCASE("delete the first band") {
        stays_between([](EqState& e) { e.bands.erase(e.bands.begin()); }, 1000.0, 0.3);
    }
    SUBCASE("insert a flat band at the front") {
        stays_between([](EqState& e) { e.bands.insert(e.bands.begin(), peaking(700.0, 0.0, 1.0, 6)); }, 300.0, 0.05);
    }
    SUBCASE("swap two bands") {
        stays_between([](EqState& e) { std::swap(e.bands[1], e.bands[2]); }, 1000.0, 0.05);
    }
    SUBCASE("sort by frequency after a band is dragged past another") {
        stays_between([](EqState& e) {
            e.bands[3].fc = 12000.0;   // past the 10 kHz band, away from the 1 kHz probe
            std::stable_sort(e.bands.begin(), e.bands.end(), [](const Band& a, const Band& b) { return a.fc < b.fc; });
        }, 1000.0, 0.3);
    }
}

TEST_CASE("a change queued behind a crossfade waits for the fade's last sample") {
    // A shape change during a crossfade starts when the running fade ends. It
    // must not start in the same control block the fade completes in, or that
    // block's ramp restarts from zero and the output steps by the part of the
    // fade that was skipped. 44.1 kHz leaves a remainder at the end of each fade.
    Setup st;
    st.fs = 44100.0;
    st.block = 64;
    auto worst_bend = [&](size_t gap) {
        double worst = 0.0;
        for (int phase = 0; phase < 8; ++phase) {
            EqState s;
            s.bands.push_back(peaking(100.0, 12.0, 1.0, 1));
            const size_t t0 = 4416 + static_cast<size_t>(phase) * 64;
            const Signal out = render(s, st, 22050, sine(100.0, st.fs), [&](size_t pos, EqState& e) {
                if (pos == t0) { e.bands[0].enabled = false; return true; }
                if (pos == t0 + gap) { e.bands[0].enabled = true; e.bands[0].type = FilterType::LowShelf; e.bands[0].width = 0.7; return true; }
                return false;
            });
            for (size_t i = t0; i + 2 < out[0].size(); ++i) {
                worst = std::max(worst, std::abs(double(out[0][i + 2]) - 2.0 * out[0][i + 1] + out[0][i]));
            }
        }
        return worst;
    };
    const double queued = worst_bend(128);   // 2.9 ms: during the first fade
    const double spaced = worst_bend(896);   // 20 ms: after it
    CAPTURE(queued);
    CAPTURE(spaced);
    CHECK(queued < spaced * 3.0);
}

TEST_CASE("a band deleted while it is still fading in is removed once that fade ends") {
    // The slot is mid-crossfade, so the removal is queued (pending_remove) and
    // runs as its own fade when the first one finishes.
    Setup setup;
    const size_t mid_fade = kChange + 240;   // 5 ms into the 10 ms fade in
    const Signal out = render(EqState{}, setup, kChange + 9600, sine(1000.0), [&](size_t pos, EqState& st) {
        if (pos == kChange) {
            st.bands = {peaking(1000.0, -12.0, 1.0, 7)};
            return true;
        }
        if (pos == mid_fade) {
            st.bands.clear();
            return true;
        }
        return false;
    });
    const double at_fade_end = level_db(out[0], kChange + 480, 1000.0, setup.fs);
    const double long_after = level_db(out[0], kChange + 9000, 1000.0, setup.fs, 10);
    CAPTURE(at_fade_end);
    CAPTURE(long_after);
    CHECK(at_fade_end < -6.0);   // the band did reach the output
    CHECK(std::abs(long_after) < 0.01);
    CHECK(worst_step(out[0]) < 2.0 * std::sin(kPi * 1000.0 / setup.fs) * 1.10);
}

TEST_CASE("bypass turns off the EQ and nothing else") {
    // Owner's decision (2026-09-13): bypass removes the bands and the preamp.
    // Mute, speaker mute, trims, polarity, routing, bass management and delay
    // stay, as an AV receiver's EQ-off keeps its speaker setup.
    Setup st;
    SUBCASE("bands and preamp are bypassed") {
        EqState s;
        s.bands.push_back(peaking(1000.0, -12.0, 1.0));
        s.preamp_db = -6.0;
        s.bypass = true;
        const Signal out = render(s, st, 24000, sine(1000.0));
        CHECK(std::abs(level_db(out[0], 20000, 1000.0, st.fs, 10)) < 0.01);
    }
    SUBCASE("mute stays") {
        EqState s;
        s.mute = true;
        s.bypass = true;
        const Signal out = render(s, st, 24000, sine(1000.0));
        CHECK(std::abs(level_db(out[0], 20000, 1000.0, st.fs, 10)) > 100.0);
    }
    SUBCASE("speaker mute, trims and polarity stay") {
        EqState s;
        s.speakers.muted = 2;
        s.channel_gain_db[0] = -6.0;
        s.speakers.inverted = 1;
        s.bypass = true;
        const Signal out = render(s, st, 24000, sine(1000.0));
        CHECK(std::abs(in_phase(out[0], 20000, 1000.0, st.fs) - -std::pow(10.0, -6.0 / 20.0)) < 0.001);
        CHECK(std::abs(level_db(out[1], 20000, 1000.0, st.fs, 10)) > 100.0);
    }
    SUBCASE("bass management stays") {
        enum { FL, FR, FC, LFE, SL, SR };
        st.channels = 6;
        st.mask = 0x60F;
        EqState s;
        s.speakers.bass_management = true;
        s.speakers.small_speakers = ChannelMask{1} << FL;
        s.bypass = true;
        const Signal out = render(s, st, 48000, [](uint32_t c, size_t i) { return c == FL ? std::sin(2.0 * kPi * 40.0 * i / 48000.0) : 0.0; });
        CHECK(std::abs(level_db(out[FL], 40000, 40.0, st.fs, 10) - lr4_db(FilterType::HighPass, 80.0, 40.0, st.fs)) < 0.05);
    }
    SUBCASE("delay stays") {
        EqState s;
        s.speakers.delay_ms[0] = 10.0;
        s.bypass = true;
        const Signal out = render(s, st, 2400, [](uint32_t, size_t i) { return i == 100 ? 1.0 : 0.0; });
        CHECK(out[0][580] == doctest::Approx(1.0f));
        CHECK(out[0][100] == 0.0f);
    }
    SUBCASE("routing stays") {
        EqState s;
        s.speakers.swap_left_right = true;
        s.bypass = true;
        const Signal out = render(s, st, 2400, [](uint32_t c, size_t) { return c == 0 ? 0.25 : 0.75; });
        CHECK(out[0][2000] == doctest::Approx(0.75f));
    }
}

TEST_CASE("a NaN on one input reaches only the outputs it is routed to") {
    // With a routing matrix active, a zero entry must not multiply the NaN into
    // every output (0 * NaN is NaN), which would reset every channel's filters.
    Setup st;
    st.channels = 8;
    st.mask = 0x63F;
    EqState s;
    s.speakers.swap_left_right = true;
    s.bands.push_back(peaking(50.0, 12.0, 4.0));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto input = [nan](bool with_nan) {
        return [=](uint32_t c, size_t i) {
            if (with_nan && c == 1 && i == 4800) return nan;
            return std::sin(2.0 * kPi * 50.0 * static_cast<double>(i) / 48000.0 + c);
        };
    };
    const Signal clean = render(s, st, 14400, input(false));
    const Signal hit = render(s, st, 14400, input(true));
    for (uint32_t c = 2; c < 8; ++c) {
        CAPTURE(c);
        double worst = 0.0;
        for (size_t i = 0; i < clean[c].size(); ++i) worst = std::max(worst, std::abs(double(clean[c][i]) - hit[c][i]));
        CHECK(worst == 0.0);
    }
}

TEST_CASE("gains that multiply past a float's range never reach the output as inf") {
    Setup st;
    EqState s;
    for (int i = 0; i < 12; ++i) s.bands.push_back(peaking(1000.0, 60.0, 1.0, static_cast<uint32_t>(i + 1)));
    s.preamp_db = 60.0;
    s.channel_gain_db[0] = 60.0;
    const Signal out = render(s, st, 48000, sine(1000.0, 48000.0, 0.5));
    bool finite = true;
    for (float v : out[0]) finite &= std::isfinite(v);
    CHECK(finite);
}
