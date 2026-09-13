// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Defects found by the 2026-09-13 code review, one test each. Every test here
// fails on the code before the fix (checked by reverting each fix).

#include "doctest.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/audio_ring.h"
#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/response.h"
#include "isotone/speakers.h"

using namespace isotone;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

Band peaking(double fc, double gain_db, double q) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    return b;
}

double sine_step_limit(double amplitude, double freq) {
    return amplitude * 2.0 * std::sin(kPi * freq / kFs);
}

double worst_step(const std::vector<float>& x) {
    double worst = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    }
    return worst;
}

struct Stereo {
    std::vector<float> l, r;
};

// A 1 kHz unit sine on both channels, `mutate(pos)` before each block, `amp`
// scaling the input. Returns both channels.
template <typename F>
Stereo render(EqState& s, double seconds, uint32_t block, F&& mutate, double freq = 1000.0,
              double amp = 1.0) {
    Processor p;
    p.initialize(kFs, 2, block);
    p.set_target(s);
    p.reset();
    const uint32_t total = static_cast<uint32_t>(kFs * seconds);
    Stereo out;
    std::vector<float> l(block), r(block);
    float* ptr[2] = {l.data(), r.data()};
    for (uint32_t pos = 0; pos < total; pos += block) {
        if (mutate(pos, l, r)) {
            p.set_target(s);
        }
        for (uint32_t i = 0; i < block; ++i) {
            l[i] = r[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * (pos + i) / kFs));
        }
        mutate.after_fill(pos, l, r);
        p.process(ptr, block);
        out.l.insert(out.l.end(), l.begin(), l.end());
        out.r.insert(out.r.end(), r.begin(), r.end());
    }
    return out;
}

// Level of a sine at `freq` over [from, to) samples.
double level_db(const std::vector<float>& x, size_t from, size_t to, double freq) {
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    const size_t n = to - from;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        acc += static_cast<double>(x[from + i]) * w * std::polar(1.0, -2.0 * kPi * freq * i / kFs);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::abs(acc) / wsum);
}

struct Mutation {
    std::function<bool(uint32_t)> change;
    std::function<void(uint32_t, std::vector<float>&, std::vector<float>&)> fill;
    bool operator()(uint32_t pos, std::vector<float>&, std::vector<float>&) { return change(pos); }
    void after_fill(uint32_t pos, std::vector<float>& l, std::vector<float>& r) {
        if (fill) fill(pos, l, r);
    }
};

}  // namespace

TEST_CASE("moving a band to another channel crossfades on both channels") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    s.bands[0].channels = ChannelMask{1} << 0;   // left

    bool moved = false;
    Mutation m{[&](uint32_t pos) {
        // 9664 is 1.33 cycles past a whole second: the switch lands near a
        // peak, not on a zero crossing where a jump would be invisible.
        if (!moved && pos >= 9650) {
            s.bands[0].channels = ChannelMask{1} << 1;   // right
            moved = true;
            return true;
        }
        return false;
    }, nullptr};
    const Stereo out = render(s, 0.5, 64, m);
    const double limit = sine_step_limit(std::pow(10.0, 12.0 / 20.0), 1000.0) * 1.10;
    CAPTURE(worst_step(out.l));
    CAPTURE(worst_step(out.r));
    CHECK(worst_step(out.l) < limit);
    CHECK(worst_step(out.r) < limit);
    CHECK(level_db(out.l, out.l.size() - 4800, out.l.size(), 1000.0) == doctest::Approx(0.0).epsilon(0.01));
    CHECK(level_db(out.r, out.r.size() - 4800, out.r.size(), 1000.0) == doctest::Approx(12.0).epsilon(0.001));
}

TEST_CASE("re-enabling a band while it is still fading out does not click") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 12.0, 1.0));
    int stage = 0;
    const uint32_t off_at = 9616;   // a third of a cycle past a zero crossing
    Mutation m{[&](uint32_t pos) {
        if (stage == 0 && pos >= off_at) {
            s.bands[0].enabled = false;
            stage = 1;
            return true;
        }
        if (stage == 1 && pos >= off_at + 96) {   // 2 ms later, mid-fade
            s.bands[0].enabled = true;
            stage = 2;
            return true;
        }
        return false;
    }, nullptr};
    const Stereo out = render(s, 0.5, 16, m);
    CAPTURE(worst_step(out.l));
    CHECK(worst_step(out.l) < sine_step_limit(std::pow(10.0, 12.0 / 20.0), 1000.0) * 1.10);
    CHECK(stage == 2);
    CHECK(level_db(out.l, out.l.size() - 4800, out.l.size(), 1000.0) == doctest::Approx(12.0).epsilon(0.001));
}

TEST_CASE("smoothing takes the same time whatever size the host's buffers are") {
    const auto run = [](uint32_t block) {
        EqState s;
        s.bands.push_back(peaking(1000.0, 0.0, 1.0));
        bool changed = false;
        Mutation m{[&](uint32_t pos) {
            if (!changed && pos >= 4800) {
                s.bands[0].gain_db = -12.0;
                changed = true;
                return true;
            }
            return false;
        }, nullptr};
        // Changes land on a block boundary; 4800 is one for every size used.
        return render(s, 0.3, block, m).l;
    };
    const std::vector<float> small = run(1);
    const std::vector<float> large = run(480);
    double worst = 0.0;
    for (size_t i = 0; i < std::min(small.size(), large.size()); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(small[i]) - large[i]));
    }
    CAPTURE(worst);
    // The committed code before the fix is off by ~0.7 here: a 1-frame buffer
    // advanced the smoother a whole control block per sample.
    CHECK(worst < 0.05);
}

TEST_CASE("a huge finite gain cannot leave the filters NaN, and a sane block recovers") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 0.0, 1.0));
    s.bands.push_back(peaking(1000.0, 0.0, 1.0));
    int stage = 0;
    Mutation m{[&](uint32_t pos) {
        if (stage == 0 && pos >= 4800) {
            s.preamp_db = std::numeric_limits<float>::max();
            s.bands[0].gain_db = 1e30;
            s.bands[1].gain_db = 1e30;
            stage = 1;
            return true;
        }
        if (stage == 1 && pos >= 24000) {
            s.preamp_db = 0.0;
            s.bands[0].gain_db = 0.0;
            s.bands[1].gain_db = 0.0;
            stage = 2;
            return true;
        }
        return false;
    }, nullptr};
    const Stereo out = render(s, 1.5, 64, m);
    bool finite = true;
    for (float v : out.l) finite &= std::isfinite(v);
    CHECK(finite);
    CHECK(level_db(out.l, out.l.size() - 9600, out.l.size(), 1000.0) == doctest::Approx(0.0).epsilon(0.001));
}

TEST_CASE("preamp, trims and band gain are clamped to a finite range") {
    {
        EqState s;
        s.bands.push_back(peaking(1000.0, 1000.0, 1.0));
        Mutation m{[](uint32_t) { return false; }, nullptr};
        const Stereo out = render(s, 0.3, 64, m, 1000.0, 1e-4);
        CHECK(level_db(out.l, 9600, out.l.size(), 1000.0) == doctest::Approx(-80.0 + kMaxBandGainDb).epsilon(0.001));
    }
    EqState s;
    s.preamp_db = 1000.0;
    s.channel_gain_db[1] = -1000.0;
    Mutation m{[](uint32_t) { return false; }, nullptr};
    const Stereo out = render(s, 0.3, 64, m, 1000.0, 1e-4);
    CHECK(level_db(out.l, 9600, out.l.size(), 1000.0) == doctest::Approx(-80.0 + kMaxLevelDb).epsilon(0.001));
    CHECK(level_db(out.r, 9600, out.r.size(), 1000.0) ==
          doctest::Approx(-80.0 + kMaxLevelDb + kMinLevelDb).epsilon(0.001));
}

// At 50 Hz a sine moves only 0.0065 per sample, so a gain that steps once per
// 32-sample control block shows up as a step far larger than the sine's own.
TEST_CASE("a preamp change is ramped per sample, not per control block") {
    EqState s;
    bool changed = false;
    Mutation m{[&](uint32_t pos) {
        if (!changed && pos >= 9600) {
            s.preamp_db = -12.0;
            changed = true;
            return true;
        }
        return false;
    }, nullptr};
    const Stereo out = render(s, 0.3, 480, m, 50.0);
    CAPTURE(worst_step(out.l));
    CHECK(worst_step(out.l) < sine_step_limit(1.0, 50.0) * 1.10);
}

TEST_CASE("a crossfade weight is ramped per sample, not per control block") {
    EqState s;
    s.bands.push_back(peaking(50.0, 12.0, 1.0));
    bool changed = false;
    Mutation m{[&](uint32_t pos) {
        if (!changed && pos >= 9600) {
            s.bands[0].enabled = false;
            changed = true;
            return true;
        }
        return false;
    }, nullptr};
    const Stereo out = render(s, 0.3, 480, m, 50.0);
    CAPTURE(worst_step(out.l));
    CHECK(worst_step(out.l) < sine_step_limit(std::pow(10.0, 12.0 / 20.0), 50.0) * 1.10);
}

TEST_CASE("a non-finite input sample does not silence the channel for good") {
    EqState s;
    s.bands.push_back(peaking(1000.0, -6.0, 1.0));
    Mutation m{[](uint32_t) { return false; },
               [](uint32_t pos, std::vector<float>& l, std::vector<float>&) {
                   if (pos == 4800) l[3] = std::numeric_limits<float>::quiet_NaN();
               }};
    const Stereo out = render(s, 0.5, 64, m);
    bool finite = true;
    for (float v : out.l) finite &= std::isfinite(v);
    CHECK(finite);
    CHECK(level_db(out.l, out.l.size() - 4800, out.l.size(), 1000.0) == doctest::Approx(-6.0).epsilon(0.001));
}

TEST_CASE("a band with no width is off in the audio, as it is in the drawn curve") {
    EqState s;
    s.bands.push_back(peaking(1000.0, 6.0, 0.0));
    Mutation m{[](uint32_t) { return false; }, nullptr};
    const Stereo out = render(s, 0.3, 64, m);
    const double freq = 1000.0;
    double drawn = 0.0;
    magnitude_db(s, 0, &freq, 1, kFs, &drawn);
    CHECK(drawn == doctest::Approx(0.0));
    CHECK(level_db(out.l, 4800, out.l.size(), 1000.0) == doctest::Approx(drawn).epsilon(0.001));
}

TEST_CASE("changing a band's width mode does not sweep through widths of the other unit") {
    // A 12 dB/oct shelf becomes Q 0.7. Without snapping, the width smoother
    // would carry 12 (a slope) toward 0.7 (a Q) for ~100 ms, designing a
    // resonant shelf nobody asked for. 60 ms after the change the level must
    // already be the new filter's.
    EqState s;
    Band shelf;
    shelf.type = FilterType::LowShelf;
    shelf.fc = 200.0;
    shelf.gain_db = 12.0;
    shelf.width = 12.0;
    shelf.width_mode = WidthMode::SlopeDb;
    s.bands.push_back(shelf);
    const uint32_t at = 9600;
    bool changed = false;
    Mutation m{[&](uint32_t pos) {
        if (!changed && pos >= at) {
            s.bands[0].width = 0.7;
            s.bands[0].width_mode = WidthMode::Q;
            changed = true;
            return true;
        }
        return false;
    }, nullptr};
    const double f = 180.0;
    const Stereo out = render(s, 0.3, 64, m, f);
    const BiquadCoeffs final_filter = design(s.bands[0], kFs);
    const double expect = 20.0 * std::log10(std::abs(response(final_filter, f, kFs)));
    const size_t from = at + static_cast<size_t>(kFs * 0.030);
    CHECK(level_db(out.l, from, from + static_cast<size_t>(kFs * 0.030), f) ==
          doctest::Approx(expect).epsilon(0.002));
}

TEST_CASE("a steep shelf slope stays a stable filter at high gain") {
    Band b;
    b.type = FilterType::LowShelf;
    b.fc = 100.0;
    b.gain_db = 30.0;
    b.width = 24.0;
    b.width_mode = WidthMode::SlopeDb;
    const BiquadCoeffs c = design(b, kFs);
    const BiquadCoeffs id = BiquadCoeffs::identity();
    CHECK_FALSE((c.b0 == id.b0 && c.b1 == id.b1 && c.b2 == id.b2 && c.a1 == id.a1 && c.a2 == id.a2));
    // Poles inside the unit circle: |a2| < 1 and |a1| < 1 + a2. The clamp holds
    // them a margin inside, not merely off the circle: a floor near zero instead
    // of the value for Q 10 still prints a2 as 1.000000 and passes |a2| < 1.
    CHECK(std::abs(c.a2) < 0.9999);
    CHECK(std::abs(c.a1) < 1.0 + c.a2);
    CHECK(std::abs(20.0 * std::log10(std::abs(response(c, 1.0, kFs))) - 30.0) < 0.05);
}

TEST_CASE("the ring reader uses the capacity it was given, not the header's") {
    const uint32_t kCap = 64;
    std::vector<float> storage(1 + kCap * kMaxChannels + 64, 0.0f);
    auto* ring = reinterpret_cast<AudioRingHeader*>(storage.data());
    audio_ring_init(ring, kCap);
    AudioRingWriter w;
    w.attach(ring, kCap);
    w.set_channels(2);
    REQUIRE(w.claim((uint64_t{7} << 32) | 1));
    std::vector<float> out(kCap * kMaxChannels);
    uint32_t ch = 0;
    AudioRingCursor cursor;
    audio_ring_read(ring, kCap, &cursor, out.data(), kCap, &ch);
    std::vector<float> frames(20, 1.0f);
    w.write(frames.data(), 2, 10);
    // Another user rewrites the capacity to a larger power of two.
    ring->capacity = 1u << 20;
    CHECK(audio_ring_read(ring, kCap, &cursor, out.data(), kCap, &ch) == 0);
    ring->capacity = kCap;
    CHECK(audio_ring_read(ring, kCap, &cursor, out.data(), kCap, &ch) == 10);
}

TEST_CASE("a writer that lost its claim to another process stops writing") {
    const uint32_t kCap = 64;
    std::vector<float> storage(1 + kCap * kMaxChannels + 64, 0.0f);
    auto* ring = reinterpret_cast<AudioRingHeader*>(storage.data());
    audio_ring_init(ring, kCap);
    AudioRingWriter a, b;
    a.attach(ring, kCap);
    b.attach(ring, kCap);
    a.set_channels(2);
    b.set_channels(2);
    REQUIRE(a.claim((uint64_t{100} << 32) | 1));
    // A different process takes it over once a has stopped writing.
    for (uint32_t i = 0; i < kRingStaleClaims; ++i) b.claim((uint64_t{200} << 32) | 1);
    REQUIRE(b.claim((uint64_t{200} << 32) | 1));
    std::vector<float> frames(20, 1.0f);
    b.write(frames.data(), 2, 10);
    a.write(frames.data(), 2, 7);
    CHECK_FALSE(a.owns());
    CHECK(b.owns());
    CHECK(ring->write_index == 10);
}

TEST_CASE("frequencies are written so Equalizer APO's Room EQ Wizard rule cannot misread them") {
    CHECK(format_apo_frequency(80.125) == "80.1250");
    CHECK(format_apo_frequency(1234.567) == "1234.5670");
    CHECK(format_apo_frequency(1000.0) == "1000");
    CHECK(format_apo_frequency(80.5) == "80.5");
    EqState s;
    s.bands.push_back(peaking(80.125, -3.0, 1.0));
    s.bands.push_back(peaking(1234.567, 2.0, 1.0));
    const ApoParseResult r = parse_apo_config(format_apo_config(s));
    REQUIRE(r.state.bands.size() == 2);
    CHECK(r.state.bands[0].fc == doctest::Approx(80.125));
    CHECK(r.state.bands[1].fc == doctest::Approx(1234.567));
    // The rule itself still applies to text written by Room EQ Wizard.
    CHECK(parse_apo_config("Filter 1: ON PK Fc 1.000 Hz Gain 1 dB Q 1\n").state.bands[0].fc == doctest::Approx(1000.0));
}

TEST_CASE("for a device's sample rate, frequencies are written as the processor designs them") {
    // Upstream does not clamp: a band above Nyquist makes its biquad unstable
    // and Equalizer APO outputs silence, where the processor designs it at
    // 0.95 of Nyquist.
    EqState s;
    s.bands.push_back(peaking(80125.0, -12.0, 2.0));
    s.bands.push_back(peaking(3.0, 3.0, 1.0));
    s.bands.push_back(peaking(1000.0, 3.0, 1.0));
    ApoFormatOptions device;
    device.sample_rate = 48000.0;
    const ApoParseResult r = parse_apo_config(format_apo_config(s, device));
    REQUIRE(r.state.bands.size() == 3);
    CHECK(r.state.bands[0].fc == doctest::Approx(clamp_fc(80125.0, 48000.0)));
    CHECK(r.state.bands[1].fc == doctest::Approx(kMinFc));
    CHECK(r.state.bands[2].fc == doctest::Approx(1000.0));
    // An export is for no particular device and keeps what was entered.
    CHECK(parse_apo_config(format_apo_config(s)).state.bands[0].fc == doctest::Approx(80125.0));
}

TEST_CASE("numbers are read and written with a period whatever the C locale") {
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous ? previous : "C";
    const char* german = std::setlocale(LC_NUMERIC, "de-DE");
    if (german == nullptr) german = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    if (german == nullptr) german = std::setlocale(LC_NUMERIC, "German");
    if (german == nullptr) {
        MESSAGE("no comma-decimal locale installed; locale independence not exercised");
        return;
    }
    char probe[16];
    std::snprintf(probe, sizeof(probe), "%.1f", 1.5);
    CAPTURE(probe);
    CHECK(format_apo_number(1419.85) == "1419.85");
    CHECK(format_apo_config(EqState{}).find(',') == std::string::npos);
    const ApoParseResult r = parse_apo_config("Filter 1: ON PK Fc 1419.85 Hz Gain -3.5 dB Q 1.25\n");
    REQUIRE(r.state.bands.size() == 1);
    CHECK(r.state.bands[0].fc == doctest::Approx(1419.85));
    CHECK(r.state.bands[0].gain_db == doctest::Approx(-3.5));
    SpeakerSetup sp;
    sp.delay_ms[1] = 1.5;
    SpeakerSetup back;
    std::string error;
    CHECK(parse_speaker_setup(format_speaker_setup(sp), &back, &error));
    CHECK(back.delay_ms[1] == 1.5);
    std::setlocale(LC_NUMERIC, saved.c_str());
}

TEST_CASE("width forms the importer does not accept are exported as the Q that designs the same filter") {
    EqState s;
    Band shelf;
    shelf.type = FilterType::HighShelf;
    shelf.fc = 3000.0;
    shelf.gain_db = 5.0;
    shelf.width = 1.2;
    shelf.width_mode = WidthMode::BandwidthOct;
    s.bands.push_back(shelf);
    Band pk = peaking(500.0, -4.0, 2.5);
    pk.width_mode = WidthMode::SlopeDb;   // design() reads this as Q 2.5
    s.bands.push_back(pk);
    Band lp;
    lp.type = FilterType::LowPass;
    lp.fc = 12000.0;
    lp.width = 0.9;
    lp.width_mode = WidthMode::SlopeDb;
    s.bands.push_back(lp);

    const ApoParseResult r = parse_apo_config(format_apo_config(s));
    REQUIRE(r.warnings.empty());
    REQUIRE(r.state.bands.size() == 3);
    const std::vector<double> grid = log_grid(20.0, 20000.0, 128);
    std::vector<double> a(grid.size()), b(grid.size());
    magnitude_db(s, 0, grid.data(), grid.size(), kFs, a.data());
    magnitude_db(r.state, 0, grid.data(), grid.size(), kFs, b.data());
    for (size_t i = 0; i < grid.size(); ++i) {
        CAPTURE(grid[i]);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-6));
    }
}

TEST_CASE("a bandwidth shelf with the corner flag exports as the filter the processor designs") {
    // The processor ignores the corner shift for a bandwidth width, as upstream
    // does, so the exported line must not ask for it: LS with a Q would shift.
    // With a device rate, the bandwidth is converted to Q at that rate.
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        EqState s;
        Band shelf;
        shelf.type = FilterType::LowShelf;
        shelf.fc = 1000.0;
        shelf.gain_db = 12.0;
        shelf.width = 1.0;
        shelf.width_mode = WidthMode::BandwidthOct;
        shelf.shelf_corner = true;
        s.bands.push_back(shelf);
        ApoFormatOptions device;
        device.sample_rate = fs;
        const ApoParseResult r = parse_apo_config(format_apo_config(s, device));
        REQUIRE(r.state.bands.size() == 1);
        const std::vector<double> grid = log_grid(20.0, 20000.0, 128);
        std::vector<double> a(grid.size()), b(grid.size());
        magnitude_db(s, 0, grid.data(), grid.size(), fs, a.data());
        magnitude_db(r.state, 0, grid.data(), grid.size(), fs, b.data());
        double worst = 0.0;
        for (size_t i = 0; i < grid.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
        CAPTURE(fs);
        CHECK(worst < 0.001);
    }
}

TEST_CASE("the export never writes a filter Equalizer APO would design unstable or NaN") {
    // Upstream takes the square root of a negative number for a dB slope past
    // what the processor holds, writes a band with no width as a real filter,
    // and reads `nan` as a number. The processor turns each of these into a
    // stable filter, identity, or the previous value; the text must not play
    // something else (review 2026-09-13).
    SUBCASE("a dB slope too steep for its gain is written at the steepest stable slope") {
        EqState s;
        Band hs;
        hs.type = FilterType::HighShelf;
        hs.fc = 3000.0;
        hs.gain_db = 24.0;
        hs.width = 24.0;
        hs.width_mode = WidthMode::SlopeDb;
        s.bands.push_back(hs);
        const ApoParseResult r = parse_apo_config(format_apo_config(s));
        REQUIRE(r.state.bands.size() == 1);
        const double slope = r.state.bands[0].width;
        const double A = std::pow(10.0, 24.0 / 40.0);
        const double inner = (A + 1.0 / A) * (12.0 / slope - 1.0) + 2.0;
        CHECK(inner >= 0.01 - 1e-9);
        CHECK(slope < 24.0);
        // And it is the filter the processor designs for the original slope.
        const std::vector<double> grid = log_grid(20.0, 20000.0, 64);
        std::vector<double> a(grid.size()), b(grid.size());
        magnitude_db(s, 0, grid.data(), grid.size(), kFs, a.data());
        magnitude_db(r.state, 0, grid.data(), grid.size(), kFs, b.data());
        for (size_t i = 0; i < grid.size(); ++i) CHECK(std::abs(a[i] - b[i]) < 0.001);
    }
    SUBCASE("a band with no width is written off") {
        EqState s;
        Band lp;
        lp.type = FilterType::LowPass;
        lp.fc = 2000.0;
        lp.width = 0.0;
        s.bands.push_back(lp);
        Band pk = peaking(1000.0, 6.0, -1.0);
        s.bands.push_back(pk);
        const std::string text = format_apo_config(s);
        CAPTURE(text);
        CHECK(text.find(": ON ") == std::string::npos);
    }
    SUBCASE("non-finite values are not written, and levels are clamped as the processor clamps them") {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        EqState s;
        s.preamp_db = nan;
        s.channel_gain_db[1] = 500.0;
        s.bands.push_back(peaking(nan, 3.0, 1.0));
        s.bands.push_back(peaking(1000.0, 1e9, 1.0));
        const std::string text = format_apo_config(s);
        CAPTURE(text);
        CHECK(text.find("nan") == std::string::npos);
        CHECK(text.find("inf") == std::string::npos);
        const ApoParseResult r = parse_apo_config(text);
        CHECK(r.state.preamp_db == 0.0);
        CHECK(r.state.channel_gain_db[1] == doctest::Approx(kMaxLevelDb));
        REQUIRE(r.state.bands.size() == 1);
        CHECK(r.state.bands[0].gain_db == doctest::Approx(kMaxBandGainDb));
    }
}

TEST_CASE("mute is exported as silence and read back as mute") {
    EqState s;
    s.mute = true;
    s.channel_gain_db[1] = -2.0;
    s.bands.push_back(peaking(1000.0, 3.0, 1.0));
    ChannelLayout layout;
    layout.channels = 6;
    layout.speaker_mask = 0x60F;
    ApoFormatOptions options;
    options.layout = layout;
    const std::string text = format_apo_config(s, options);
    CAPTURE(text);
    CHECK(text.find("-100") == std::string::npos);
    CHECK(text.find("Copy: L=0 R=0 C=0 LFE=0 SL=0 SR=0") != std::string::npos);
    const ApoParseResult r = parse_apo_config(text, layout);
    CHECK(r.state.mute);
    CHECK(r.unsupported.empty());
    CHECK(r.state.channel_gain_db[1] == doctest::Approx(-2.0));

    // Anything short of zeroing every channel is not mute.
    const ApoParseResult partial = parse_apo_config("Copy: L=0 R=0\n", layout);
    CHECK_FALSE(partial.state.mute);
    CHECK(partial.unsupported.size() == 1);
    const ApoParseResult swap = parse_apo_config("Copy: L=R R=L\n");
    CHECK_FALSE(swap.state.mute);
}
