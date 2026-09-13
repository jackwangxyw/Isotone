// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The shared-memory schema and its seqlock. The concurrency test runs a real
// writer thread against a real reader thread, because a seqlock that is only
// tested single-threaded proves nothing.

#include "doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/response.h"

using namespace isotone;

namespace {

Band peaking(double fc, double gain_db, double q) {
    Band b;
    b.type = FilterType::Peaking;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    return b;
}

}  // namespace

TEST_CASE("the layout is fixed and self-describing") {
    ParamBlock b{};
    init_param_block(&b);
    CHECK(b.hdr.magic == kParamMagic);
    CHECK(b.hdr.version == kParamVersion);
    CHECK(b.hdr.size == sizeof(ParamBlock));
    CHECK(param_block_valid(b));

    // A reader must reject a block it does not understand rather than misread it.
    ParamBlock wrong = b;
    wrong.hdr.magic = 0;
    CHECK_FALSE(param_block_valid(wrong));

    wrong = b;
    wrong.hdr.version = kParamVersion + 1;
    CHECK_FALSE(param_block_valid(wrong));

    wrong = b;
    wrong.hdr.size = 1;
    CHECK_FALSE(param_block_valid(wrong));

    wrong = b;
    wrong.band_count = kParamMaxBands + 1;
    CHECK_FALSE(param_block_valid(wrong));
}

TEST_CASE("an EqState survives a trip through the block") {
    EqState s;
    s.bypass = true;
    s.mute = false;
    s.preamp_db = -6.5;
    s.channel_gain_db[0] = -1.5;
    s.channel_gain_db[1] = 2.25;

    Band a = peaking(1000.0, -12.0, 1.5);
    a.id = 42;
    a.channels = (ChannelMask{1} << 0);
    s.bands.push_back(a);

    Band shelf;
    shelf.type = FilterType::HighShelf;
    shelf.fc = 8000.0;
    shelf.gain_db = 3.0;
    shelf.width = 0.9;
    shelf.width_mode = WidthMode::SlopeDb;
    shelf.shelf_corner = true;
    shelf.id = 43;
    s.bands.push_back(shelf);

    Band off = peaking(300.0, 5.0, 2.0);
    off.enabled = false;
    off.id = 44;
    s.bands.push_back(off);

    ParamBlock block{};
    init_param_block(&block);
    to_param_block(s, &block);

    EqState back;
    from_param_block(block, &back);

    CHECK(back.bypass == s.bypass);
    CHECK(back.mute == s.mute);
    CHECK(back.preamp_db == doctest::Approx(s.preamp_db));
    CHECK(back.channel_gain_db[0] == doctest::Approx(s.channel_gain_db[0]));
    CHECK(back.channel_gain_db[1] == doctest::Approx(s.channel_gain_db[1]));
    REQUIRE(back.bands.size() == 3);

    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(back.bands[i].id == s.bands[i].id);
        CHECK(back.bands[i].type == s.bands[i].type);
        CHECK(back.bands[i].width_mode == s.bands[i].width_mode);
        CHECK(back.bands[i].channels == s.bands[i].channels);
        CHECK(back.bands[i].enabled == s.bands[i].enabled);
        CHECK(back.bands[i].shelf_corner == s.bands[i].shelf_corner);
        CHECK(back.bands[i].fc == doctest::Approx(s.bands[i].fc).epsilon(1e-6));
        CHECK(back.bands[i].gain_db == doctest::Approx(s.bands[i].gain_db).epsilon(1e-6));
        CHECK(back.bands[i].width == doctest::Approx(s.bands[i].width).epsilon(1e-6));
    }
}

TEST_CASE("the curve is preserved across the block within float precision") {
    EqState s;
    s.preamp_db = -6.1;
    s.bands.push_back(peaking(8800.0, 5.1, 1.42));
    s.bands.push_back(peaking(118.0, -3.1, 0.50));

    Band lsc;
    lsc.type = FilterType::LowShelf;
    lsc.fc = 105.0;
    lsc.gain_db = 6.4;
    lsc.width = 0.70;
    s.bands.push_back(lsc);

    ParamBlock block{};
    init_param_block(&block);
    to_param_block(s, &block);
    EqState back;
    from_param_block(block, &back);

    const std::vector<double> grid = log_grid(20.0, 20000.0, 256);
    std::vector<double> a(grid.size()), b(grid.size());
    magnitude_db(s, 0, grid.data(), grid.size(), 48000.0, a.data());
    magnitude_db(back, 0, grid.data(), grid.size(), 48000.0, b.data());
    for (size_t i = 0; i < grid.size(); ++i) {
        CAPTURE(grid[i]);
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-5));
    }
}

TEST_CASE("bands beyond the block capacity are dropped, not overflowed") {
    EqState s;
    for (uint32_t i = 0; i < kParamMaxBands + 20; ++i) {
        Band b = peaking(100.0 + i, 1.0, 1.0);
        b.id = i;
        s.bands.push_back(b);
    }

    ParamBlock block{};
    init_param_block(&block);
    to_param_block(s, &block);
    CHECK(block.band_count == kParamMaxBands);
    CHECK(param_block_valid(block));

    EqState back;
    from_param_block(block, &back);
    CHECK(back.bands.size() == kParamMaxBands);
    CHECK(back.bands.back().id == kParamMaxBands - 1);
}

TEST_CASE("a corrupt type or width mode is clamped rather than trusted") {
    ParamBlock block{};
    init_param_block(&block);
    block.band_count = 1;
    block.bands[0].type = 9999;
    block.bands[0].width_mode = 9999;
    block.bands[0].fc = 1000.0f;
    block.bands[0].width = 1.0f;
    block.bands[0].flags = kBandFlagEnabled;

    EqState out;
    from_param_block(block, &out);
    REQUIRE(out.bands.size() == 1);
    CHECK(static_cast<uint32_t>(out.bands[0].type) <= 7);
    CHECK(static_cast<uint32_t>(out.bands[0].width_mode) <= 2);
}

TEST_CASE("a block of non-finite values cannot poison the host, and a sane block recovers it") {
    // The mapping is writable by any authenticated user, so the host has to
    // survive whatever lands in it. Non-finite parameters are the dangerous
    // kind: a smoother whose target is NaN becomes NaN and never comes back.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    ParamBlock block{};
    init_param_block(&block);
    block.preamp_db = nan;
    block.channel_gain_db[0] = inf;
    block.channel_gain_db[1] = -inf;
    block.band_count = 3;
    block.bands[0] = {1, 0, 0, 0, nan, inf, nan, kBandFlagEnabled};
    block.bands[1] = {2, 0, 0, 0, inf, -inf, -1.0f, kBandFlagEnabled};
    block.bands[2] = {3, 6, 0, 0, -inf, nan, inf, kBandFlagEnabled};

    EqState hostile;
    from_param_block(block, &hostile);

    constexpr double kRate = 48000.0;
    constexpr uint32_t kBlock = 480;
    Processor p;
    p.initialize(kRate, 2, kBlock);
    p.set_target(hostile);

    std::vector<float> buf(kBlock * 2);
    uint64_t n = 0;
    const auto run = [&](uint32_t blocks, std::vector<float>* tail) {
        bool finite = true;
        for (uint32_t k = 0; k < blocks; ++k) {
            for (uint32_t i = 0; i < kBlock; ++i, ++n) {
                const float v = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 1000.0 *
                                                                   static_cast<double>(n) / kRate));
                buf[size_t{i} * 2] = v;
                buf[size_t{i} * 2 + 1] = v;
            }
            p.process_interleaved(buf.data(), kBlock);
            for (float s : buf) finite &= std::isfinite(s);
            if (tail != nullptr && k + 1 == blocks) *tail = buf;
        }
        return finite;
    };

    CHECK(run(50, nullptr));

    EqState sane;
    sane.bands.push_back(peaking(1000.0, -6.0, 1.0));
    p.set_target(sane);
    std::vector<float> tail;
    CHECK(run(100, &tail));

    // 1 kHz through a -6 dB bell at 1 kHz, on a 0.5 amplitude sine.
    double peak = 0.0;
    for (size_t i = 0; i < tail.size(); i += 2) peak = std::max(peak, std::abs(double{tail[i]}));
    CHECK(20.0 * std::log10(peak / 0.5) == doctest::Approx(-6.0).epsilon(0.01));
}

namespace {

struct SeqlockRun {
    int accepted = 0;
    int rejected = 0;
    int torn = 0;
};

// Runs a writer thread against a reader thread. `writer_gap` is real elapsed
// time between writes, standing in for the gap between UI updates. It must be
// wall-clock rather than a spin count: a busy-wait stops being a time proxy the
// moment the machine is loaded, and made this test flaky on a busy CI box.
SeqlockRun run_seqlock(int reads, std::chrono::microseconds writer_gap) {
    ParamBlock shared{};
    init_param_block(&shared);

    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0}, rejected{0}, torn{0};

    std::thread writer([&] {
        uint32_t generation = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            param_block_write(&shared, [&](ParamBlock* b) {
                b->band_count = kParamMaxBands;
                b->preamp_db = static_cast<float>(generation);
                for (uint32_t i = 0; i < kParamMaxBands; ++i) {
                    b->bands[i].id = generation;
                    b->bands[i].fc = static_cast<float>(generation);
                    b->bands[i].gain_db = static_cast<float>(generation);
                    b->bands[i].width = 1.0f;
                    b->bands[i].flags = kBandFlagEnabled;
                }
            });
            ++generation;
            if (writer_gap.count() > 0) {
                std::this_thread::sleep_for(writer_gap);
            }
        }
    });

    std::thread reader([&] {
        ParamBlock copy{};
        for (int i = 0; i < reads; ++i) {
            if (!param_block_read(&shared, &copy)) {
                rejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
            if (copy.band_count == 0) {
                continue;
            }
            // Every field was written in the same pass, so they must all carry
            // the same generation. A mix of two generations is a torn read.
            const uint32_t g = copy.bands[0].id;
            bool consistent = copy.preamp_db == static_cast<float>(g);
            for (uint32_t k = 0; k < copy.band_count && consistent; ++k) {
                consistent = copy.bands[k].id == g &&
                             copy.bands[k].fc == static_cast<float>(g) &&
                             copy.bands[k].gain_db == static_cast<float>(g);
            }
            if (!consistent) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
        stop.store(true, std::memory_order_relaxed);
    });

    reader.join();
    writer.join();
    return {accepted.load(), rejected.load(), torn.load()};
}

}  // namespace

TEST_CASE("a reader never observes a half-written block, even under full contention") {
    // Writer hammering the block with no pause at all. A seqlock reader can be
    // starved by this and that is by design: it costs the host nothing, because
    // a failed read simply means it keeps the parameters it already had. What
    // must never happen is an accepted read that mixes two generations.
    const SeqlockRun r = run_seqlock(50000, std::chrono::microseconds{0});
    CAPTURE(r.accepted);
    CAPTURE(r.rejected);
    CHECK(r.torn == 0);
}

TEST_CASE("at a realistic update rate almost every read succeeds") {
    // The UI writes tens to a couple of hundred times a second while dragging,
    // and the host reads once per audio block. Reads should essentially always
    // get through.
    const SeqlockRun r = run_seqlock(20000, std::chrono::milliseconds{1});
    CAPTURE(r.accepted);
    CAPTURE(r.rejected);
    CHECK(r.torn == 0);
    CHECK(r.accepted > r.rejected * 10);
}

TEST_CASE("a write in progress is reported as a failed read, not a bad one") {
    ParamBlock shared{};
    init_param_block(&shared);

    // Leave the seqlock odd, as it is midway through a write.
    shared.hdr.seq = 1;

    ParamBlock out{};
    CHECK_FALSE(param_block_read(&shared, &out));

    shared.hdr.seq = 2;
    CHECK(param_block_read(&shared, &out));
}

TEST_CASE("a writer that died mid-write does not invert the lock for the next one") {
    // The UI is a separate process and can be killed between the two stores.
    // The next writer must still leave seq odd while writing and even after,
    // or every completed write would be rejected and every torn one accepted.
    ParamBlock shared{};
    init_param_block(&shared);
    shared.hdr.seq = 7;   // left odd by a crashed writer

    uint32_t during = 0;
    param_block_write(&shared, [&](ParamBlock* b) {
        during = b->hdr.seq;
        b->preamp_db = -3.0f;
    });
    CHECK((during & 1u) == 1u);
    CHECK((shared.hdr.seq & 1u) == 0u);

    ParamBlock out{};
    REQUIRE(param_block_read(&shared, &out));
    CHECK(out.preamp_db == -3.0f);
}

TEST_CASE("the header fields the host owns are left alone by the writer") {
    ParamBlock shared{};
    init_param_block(&shared);
    shared.hdr.sample_rate = 48000;
    shared.hdr.channels = 2;
    shared.hdr.host_state = static_cast<uint32_t>(HostState::Running);
    shared.hdr.host_heartbeat = 1234;

    EqState s;
    s.bands.push_back(peaking(1000.0, 3.0, 1.0));
    param_block_write(&shared, [&](ParamBlock* b) { to_param_block(s, b); });

    CHECK(shared.hdr.sample_rate == 48000);
    CHECK(shared.hdr.channels == 2);
    CHECK(shared.hdr.host_state == static_cast<uint32_t>(HostState::Running));
    CHECK(shared.hdr.host_heartbeat == 1234);
    CHECK(shared.band_count == 1);
    CHECK((shared.hdr.seq % 2) == 0);
}
