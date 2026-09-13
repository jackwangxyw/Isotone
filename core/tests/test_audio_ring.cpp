// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The post-EQ audio ring and the shared region layout. As with the seqlock, the
// important test is the threaded one: a lock-free ring that is only tested from
// one thread proves nothing about torn reads.

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "isotone/audio_ring.h"
#include "isotone/param_block.h"

using namespace isotone;

namespace {

// A ring with storage and a guard band after it, so a write that escapes the
// buffer is caught instead of silently corrupting the heap.
struct TestRing {
    static constexpr float kGuard = 12345.0f;
    static constexpr size_t kGuardSamples = 256;

    explicit TestRing(uint32_t capacity)
        : capacity(capacity),
          storage((sizeof(AudioRingHeader) + sizeof(float) - 1) / sizeof(float) +
                      size_t{capacity} * kMaxChannels + kGuardSamples,
                  0.0f) {
        audio_ring_init(header(), capacity);
        for (size_t i = storage.size() - kGuardSamples; i < storage.size(); ++i) {
            storage[i] = kGuard;
        }
    }

    AudioRingHeader* header() { return reinterpret_cast<AudioRingHeader*>(storage.data()); }

    bool guard_intact() const {
        for (size_t i = storage.size() - kGuardSamples; i < storage.size(); ++i) {
            if (storage[i] != kGuard) return false;
        }
        return true;
    }

    uint32_t capacity;
    std::vector<float> storage;
};

constexpr uint64_t token(uint32_t pid, uint32_t serial) {
    return (uint64_t{pid} << 32) | serial;
}

// Frames whose channel c holds (frame index + c * 0.25), so both order and
// channel placement are checkable.
std::vector<float> ramp(uint32_t first, uint32_t frames, uint32_t channels) {
    std::vector<float> v(size_t{frames} * channels);
    for (uint32_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < channels; ++c) {
            v[size_t{i} * channels + c] = static_cast<float>(first + i) + 0.25f * c;
        }
    }
    return v;
}

}  // namespace

TEST_CASE("frames written are the frames read, in order") {
    TestRing r(1024);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(2);
    REQUIRE(w.claim(token(1, 1)));

    AudioRingCursor cursor;
    std::vector<float> out(1024 * kMaxChannels);
    uint32_t ch = 0;
    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 1024, &ch) == 0);  // sync

    const std::vector<float> a = ramp(0, 300, 2);
    const std::vector<float> b = ramp(300, 500, 2);
    w.write(a.data(), 2, 300);
    w.write(b.data(), 2, 500);

    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 1024, &ch) == 800);
    CHECK(ch == 2);
    const std::vector<float> expect = ramp(0, 800, 2);
    CHECK(std::memcmp(out.data(), expect.data(), expect.size() * sizeof(float)) == 0);

    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 1024, &ch) == 0);  // drained
}

TEST_CASE("a new reader starts at the newest frame") {
    TestRing r(256);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(1);
    REQUIRE(w.claim(token(1, 1)));
    const std::vector<float> a = ramp(0, 100, 1);
    w.write(a.data(), 1, 100);

    AudioRingCursor cursor;
    std::vector<float> out(256 * kMaxChannels);
    uint32_t ch = 0;
    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 256, &ch) == 0);

    const std::vector<float> b = ramp(100, 10, 1);
    w.write(b.data(), 1, 10);
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 256, &ch) == 10);
    CHECK(out[0] == 100.0f);
}

TEST_CASE("a reader that falls behind gets the newest frames, never stale ones") {
    TestRing r(256);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(2);
    REQUIRE(w.claim(token(1, 1)));

    AudioRingCursor cursor;
    std::vector<float> out(256 * kMaxChannels);
    uint32_t ch = 0;
    audio_ring_read(r.header(), &cursor, out.data(), 256, &ch);

    const std::vector<float> a = ramp(0, 1000, 2);
    for (uint32_t i = 0; i < 1000; i += 70) {
        const uint32_t n = std::min<uint32_t>(70, 1000 - i);
        w.write(a.data() + size_t{i} * 2, 2, n);
    }
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 256, &ch) == 256);
    const std::vector<float> expect = ramp(1000 - 256, 256, 2);
    CHECK(std::memcmp(out.data(), expect.data(), expect.size() * sizeof(float)) == 0);

    SUBCASE("and a smaller request keeps the newest") {
        w.write(a.data(), 2, 50);   // frames 1000..1049 carry values 0..49
        REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 20, &ch) == 20);
        CHECK(out[0] == 30.0f);
    }
}

TEST_CASE("a single write longer than the ring keeps its tail") {
    TestRing r(64);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(1);
    REQUIRE(w.claim(token(1, 1)));
    AudioRingCursor cursor;
    std::vector<float> out(64 * kMaxChannels);
    uint32_t ch = 0;
    audio_ring_read(r.header(), &cursor, out.data(), 64, &ch);

    const std::vector<float> a = ramp(0, 200, 1);
    w.write(a.data(), 1, 200);
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 64, &ch) == 64);
    CHECK(out[0] == 136.0f);
    CHECK(out[63] == 199.0f);
    CHECK(r.guard_intact());
}

TEST_CASE("the frame counter wrapping past 2^32 loses nothing") {
    TestRing r(128);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(1);
    REQUIRE(w.claim(token(1, 1)));
    AudioRingCursor cursor;
    std::vector<float> out(128 * kMaxChannels);
    uint32_t ch = 0;
    audio_ring_read(r.header(), &cursor, out.data(), 128, &ch);

    // Silence writes only touch the last `capacity` frames, so this is cheap.
    w.write(nullptr, 1, 0x7FFFFFF0u);
    w.write(nullptr, 1, 0x7FFFFFF0u);   // counter now 0xFFFFFFE0
    audio_ring_read(r.header(), &cursor, out.data(), 128, &ch);
    CHECK(r.header()->write_index == 0xFFFFFFE0u);

    const std::vector<float> a = ramp(0, 100, 1);
    w.write(a.data(), 1, 100);   // crosses zero
    CHECK(r.header()->write_index == 0x00000044u);
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 128, &ch) == 100);
    const std::vector<float> expect = ramp(0, 100, 1);
    CHECK(std::memcmp(out.data(), expect.data(), expect.size() * sizeof(float)) == 0);
}

TEST_CASE("stride wider than the ring stores the leading channels") {
    TestRing r(64);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(12);   // clamped
    REQUIRE(w.claim(token(1, 1)));
    CHECK(r.header()->channels == kMaxChannels);

    AudioRingCursor cursor;
    std::vector<float> out(64 * kMaxChannels);
    uint32_t ch = 0;
    audio_ring_read(r.header(), &cursor, out.data(), 64, &ch);

    const std::vector<float> a = ramp(0, 10, 12);
    w.write(a.data(), 12, 10);
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 64, &ch) == 10);
    CHECK(ch == kMaxChannels);
    CHECK(out[kMaxChannels] == 1.0f);                 // frame 1, channel 0
    CHECK(out[kMaxChannels + 7] == 1.0f + 0.25f * 7); // frame 1, channel 7
    CHECK(r.guard_intact());
}

TEST_CASE("a layout change makes readers resynchronise instead of misreading") {
    TestRing r(256);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(2);
    REQUIRE(w.claim(token(1, 1)));
    AudioRingCursor cursor;
    std::vector<float> out(256 * kMaxChannels);
    uint32_t ch = 0;
    audio_ring_read(r.header(), &cursor, out.data(), 256, &ch);

    const std::vector<float> a = ramp(0, 50, 2);
    w.write(a.data(), 2, 50);
    w.set_channels(6);
    CHECK((r.header()->epoch & 1u) == 0);

    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 256, &ch) == 0);
    const std::vector<float> b = ramp(0, 40, 6);
    w.write(b.data(), 6, 40);
    REQUIRE(audio_ring_read(r.header(), &cursor, out.data(), 256, &ch) == 40);
    CHECK(ch == 6);
    CHECK(std::memcmp(out.data(), b.data(), b.size() * sizeof(float)) == 0);
}

TEST_CASE("only one writer at a time, and a dead process's claim can be taken over") {
    TestRing r(64);
    AudioRingWriter first, second, later;
    first.attach(r.header(), r.capacity);
    second.attach(r.header(), r.capacity);
    later.attach(r.header(), r.capacity);
    first.set_channels(1);
    second.set_channels(1);
    later.set_channels(1);

    CHECK(first.claim(token(100, 1)));
    CHECK_FALSE(second.claim(token(100, 2)));   // same process, held

    const std::vector<float> a = ramp(0, 10, 1);
    second.write(a.data(), 1, 10);
    CHECK(r.header()->write_index == 0);        // a non-owner writes nothing
    first.write(a.data(), 1, 10);
    CHECK(r.header()->write_index == 10);

    first.release();
    CHECK(r.header()->writer == 0);
    CHECK(second.claim(token(100, 2)));

    // The audio engine restarts: new process id, old claim never released.
    CHECK(later.claim(token(200, 1)));
    CHECK(r.header()->writer == token(200, 1));
}

TEST_CASE("corrupt layout fields in shared memory cannot steer a write out of bounds") {
    TestRing r(64);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(2);
    REQUIRE(w.claim(token(1, 1)));

    // Anyone who can open the mapping can scribble on these.
    r.header()->capacity = 0x7FFFFFFFu;
    r.header()->channels = 1000;
    r.header()->write_index = 0x12345678u;

    const std::vector<float> a = ramp(0, 500, 2);
    w.write(a.data(), 2, 500);
    CHECK(r.guard_intact());

    // And a reader facing the same garbage returns nothing rather than
    // reading out of bounds.
    AudioRingCursor cursor;
    std::vector<float> out(64 * kMaxChannels);
    uint32_t ch = 0;
    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 64, &ch) == 0);
    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 64, &ch) == 0);
    r.header()->capacity = 100;   // not a power of two
    r.header()->channels = 2;
    CHECK(audio_ring_read(r.header(), &cursor, out.data(), 64, &ch) == 0);
}

namespace {

// True if every frame in the chunk follows its predecessor. A torn read shows
// up as a jump, because an overwritten slot holds a frame `capacity` newer.
bool contiguous(const float* v, uint32_t frames) {
    for (uint32_t i = 1; i < frames; ++i) {
        if (v[i] != v[i - 1] + 1.0f) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("the torn-read detector catches a jump") {
    const float good[] = {5, 6, 7, 8};
    const float torn[] = {5, 6, 71, 8};
    CHECK(contiguous(good, 4));
    CHECK_FALSE(contiguous(torn, 4));
}

TEST_CASE("a reader racing a writer never receives an overwritten frame") {
    // A tiny ring and bursty writes up to twice its size, so the writer laps the
    // reader constantly. Values count frames modulo 2^24, where float32 is exact.
    constexpr uint32_t kCapacity = 64;
    TestRing r(kCapacity);
    AudioRingWriter w;
    w.attach(r.header(), r.capacity);
    w.set_channels(1);
    REQUIRE(w.claim(token(1, 1)));

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        std::mt19937 rng(7);
        std::uniform_int_distribution<uint32_t> size(1, kCapacity * 2);
        std::vector<float> chunk(kCapacity * 2);
        uint32_t frame = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint32_t n = size(rng);
            for (uint32_t i = 0; i < n; ++i) {
                chunk[i] = static_cast<float>((frame + i) & 0xFFFFFFu);
            }
            w.write(chunk.data(), 1, n);
            frame += n;
        }
    });

    AudioRingCursor cursor;
    std::vector<float> out(kCapacity * kMaxChannels);
    uint64_t frames = 0, chunks = 0, bad = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until) {
        uint32_t ch = 0;
        const uint32_t n = audio_ring_read(r.header(), &cursor, out.data(), kCapacity, &ch);
        if (n == 0) continue;
        ++chunks;
        frames += n;
        // Skip chunks spanning the 2^24 wrap of the test values.
        bool wraps = false;
        for (uint32_t i = 1; i < n; ++i) wraps |= out[i] < out[i - 1];
        if (!wraps && !contiguous(out.data(), n)) ++bad;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    CAPTURE(chunks);
    CAPTURE(frames);
    CHECK(chunks > 100);
    CHECK(bad == 0);
}

TEST_CASE("the shared region is self-describing") {
    std::vector<char> region(kSharedRegionBytes, 0);
    CHECK_FALSE(shared_region_valid(region.data(), region.size()));   // zeroed, as a new mapping is

    init_shared_region(region.data());
    CHECK(shared_region_valid(region.data(), region.size()));
    CHECK(region_params(region.data())->hdr.host_state == static_cast<uint32_t>(HostState::NotLoaded));
    CHECK(region_ring(region.data())->capacity == kRingCapacityFrames);
    CHECK(static_cast<void*>(region_ring(region.data())) ==
          static_cast<void*>(region.data() + sizeof(ParamBlock)));

    CHECK_FALSE(shared_region_valid(region.data(), region.size() - 1));
    region_ring(region.data())->capacity = 1024;
    CHECK_FALSE(shared_region_valid(region.data(), region.size()));
}

TEST_CASE("host header updates leave the parameter seqlock alone") {
    ParamBlock b{};
    init_param_block(&b);
    host_publish_format(&b, 96000, 6, HostState::Running);
    host_heartbeat(&b);
    host_heartbeat(&b);
    CHECK(b.hdr.sample_rate == 96000);
    CHECK(b.hdr.channels == 6);
    CHECK(b.hdr.host_state == static_cast<uint32_t>(HostState::Running));
    CHECK(b.hdr.host_heartbeat == 2);
    CHECK(param_block_seq(&b) == 0);
}
