// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Where an output's edits go on Linux, without a daemon: the test plays the
// daemon, creating the sink's region and writing its ring, and checks that
// DeviceLink writes what a daemon would read and reads what a daemon wrote.

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "devicelink.h"
#include "doctest.h"
#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "persisted_state.h"
#include "shared_region.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

// Per process, so a run collides with neither another run nor a live daemon.
std::string sink_name(const char* stem) {
    return std::string("uilink-") + stem + "-" + std::to_string(::getpid());
}

std::string scratch_dir() {
    return "/tmp/isotone-uilink-" + std::to_string(::getpid());
}

OutputTarget target_for(const std::string& sink) {
    OutputTarget t;
    t.guid = sink;
    t.backend = Backend::pipewire;
    t.layout = OutputLayout{2, 0x3, 48000};
    return t;
}

EqState with_preamp(double db) {
    EqState s;
    s.preamp_db = db;
    return s;
}

// What a daemon does: create the region and own its ring.
struct FakeDaemon {
    posix::SharedRegion region;
    AudioRingWriter     ring;
    std::string         name;

    explicit FakeDaemon(const std::string& sink) : name(posix::region_name(sink)) {
        posix::SharedRegion::unlink_region(name);
        REQUIRE(region.create_or_open(name) == 0);
        ring.attach(region.ring(), kRingCapacityFrames);
        ring.set_channels(2);
        REQUIRE(ring.claim(0x5eed'0001));
        host_publish_format(region.params(), 48000, 2, 0x3, HostState::Running);
    }
    ~FakeDaemon() {
        ring.release();
        region.close();
        posix::SharedRegion::unlink_region(name);
    }
};

}  // namespace

TEST_CASE("a commit reaches the region a daemon would read") {
    const std::string sink = sink_name("commit");
    FakeDaemon daemon(sink);

    DeviceLink link(scratch_dir());
    link.set_target(target_for(sink));
    CHECK(link.region_open());
    CHECK(link.commit(with_preamp(-4.5)) == kLinkOk);

    ParamBlock read{};
    REQUIRE(param_block_read(daemon.region.params(), &read));
    CHECK(read.preamp_db == doctest::Approx(-4.5));
    // The header stays the daemon's: the format it published must survive an edit.
    CHECK(read.hdr.sample_rate == 48000);
    CHECK(read.hdr.channels == 2);
}

TEST_CASE("with no daemon there is no region, and an edit says so") {
    const std::string sink = sink_name("absent");
    posix::SharedRegion::unlink_region(posix::region_name(sink));

    DeviceLink link(scratch_dir());
    link.set_target(target_for(sink));
    CHECK_FALSE(link.region_open());
    CHECK(link.commit(with_preamp(-1.0)) != kLinkOk);
}

TEST_CASE("save writes the file, and does so even with no daemon to tell") {
    const std::string sink = sink_name("save");
    std::filesystem::remove_all(scratch_dir());

    DeviceLink link(scratch_dir());
    link.set_target(target_for(sink));
    // No region: the file is still what the sink will start from, so this is not
    // a failure.
    CHECK(link.save(with_preamp(-6.0), with_preamp(-6.0)) == kLinkOk);

    ParamBlock saved{};
    REQUIRE(posix::read_persisted_state(link.saved_state_path().string(), &saved) ==
            posix::PersistedRead::Loaded);
    CHECK(saved.preamp_db == doctest::Approx(-6.0));
    std::filesystem::remove_all(scratch_dir());
}

TEST_CASE("load_current prefers the daemon's region and falls back to the file") {
    const std::string sink = sink_name("load");
    std::filesystem::remove_all(scratch_dir());

    // Saved while nothing hosts the sink.
    {
        DeviceLink link(scratch_dir());
        link.set_target(target_for(sink));
        REQUIRE(link.save(with_preamp(-2.0), with_preamp(-2.0)) == kLinkOk);

        EqState out;
        REQUIRE(link.load_current(&out));
        CHECK(out.preamp_db == doctest::Approx(-2.0));   // from the file
    }

    // A daemon comes up and is given something else: the region wins.
    {
        FakeDaemon daemon(sink);
        DeviceLink link(scratch_dir());
        link.set_target(target_for(sink));
        REQUIRE(link.commit(with_preamp(-9.0)) == kLinkOk);

        EqState out;
        REQUIRE(link.load_current(&out));
        CHECK(out.preamp_db == doctest::Approx(-9.0));
    }
    std::filesystem::remove_all(scratch_dir());
}

TEST_CASE("read_audio drains the ring the daemon writes") {
    const std::string sink = sink_name("ring");
    FakeDaemon daemon(sink);

    DeviceLink link(scratch_dir());
    link.set_target(target_for(sink));

    constexpr uint32_t kFrames = 480;
    std::vector<float> written(kFrames * 2);
    for (uint32_t i = 0; i < kFrames; ++i) {
        written[i * 2] = 0.25f;
        written[i * 2 + 1] = -0.25f;
    }
    daemon.ring.write(written.data(), 2, kFrames);

    std::vector<float> out(kFrames * kMaxChannels);
    uint32_t channels = 0;
    double rate = 0.0;
    // The first read synchronises the cursor and returns nothing, as the ring's
    // contract says; the frames after that are the ones to check.
    link.read_audio(out.data(), kFrames, &channels, &rate);
    daemon.ring.write(written.data(), 2, kFrames);
    const uint32_t got = link.read_audio(out.data(), kFrames, &channels, &rate);

    CHECK(got == kFrames);
    CHECK(channels == 2);
    CHECK(rate == doctest::Approx(48000.0));   // the daemon's published format
    CHECK(out[0] == doctest::Approx(0.25));
    CHECK(out[1] == doctest::Approx(-0.25));
}

TEST_CASE("only a pipewire output has a region") {
    const std::string sink = sink_name("backend");
    FakeDaemon daemon(sink);

    OutputTarget t = target_for(sink);
    t.backend = Backend::none;

    DeviceLink link(scratch_dir());
    link.set_target(t);
    CHECK_FALSE(link.region_open());
    CHECK(link.commit(with_preamp(-3.0)) != kLinkOk);
}
