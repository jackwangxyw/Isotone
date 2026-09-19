// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The outputs a Linux machine has, read from a PipeWire that is actually
// running. Skipped rather than failed where there is none, so the suite still
// passes on a machine or a container without a sound server.

#include <chrono>
#include <string>
#include <thread>

#include "doctest.h"
#include "pipewire_outputs.h"

using namespace isotone::ui;

namespace {

bool has(const std::vector<PipewireSink>& sinks, const std::string& name) {
    for (const PipewireSink& s : sinks)
        if (s.name == name) return true;
    return false;
}

}  // namespace

TEST_CASE("an output is shown by its short name") {
    // One laptop's four outputs share "Alder Lake PCH-P High Definition Audio
    // Controller" and differ only after it, where the sidebar cuts them off.
    CHECK(sink_display_name("HDMI 3", "Alder Lake PCH-P High Definition Audio Controller HDMI / DisplayPort 3 Output",
                            "alsa_output.x") == "HDMI 3");
    CHECK(sink_display_name(nullptr, "Isotone-test-sink", "isotone_test_hw") == "Isotone-test-sink");
    CHECK(sink_display_name("", "Isotone-test-sink", "isotone_test_hw") == "Isotone-test-sink");
    CHECK(sink_display_name(nullptr, nullptr, "isotone_test_hw") == "isotone_test_hw");
}

TEST_CASE("an output whose every route is unavailable is not connected") {
    // One laptop's card, unplugged HDMI (pw-dump, 2026-09-19): routes "[Out] HDMI3",
    // "HDMI2" and "HDMI1" available no, one per card.profile.device; "[Out] Speaker"
    // unknown, which is what a route that cannot tell says, and "[Out] Headphones"
    // no, both on the device the speakers are.
    const std::vector<CardRoute> card = {{0, false}, {1, false}, {2, false}, {3, true}, {3, false}};
    CHECK_FALSE(sink_connected(card, 0));   // HDMI 3
    CHECK_FALSE(sink_connected(card, 2));   // HDMI 1
    CHECK(sink_connected(card, 3));         // Speaker + Headphones
    // Nothing known about the card yet, or a sink that is on none: shown.
    CHECK(sink_connected({}, 0));
    CHECK(sink_connected(card, 7));
}

TEST_CASE("the sinks PipeWire reports are the ones it has") {
    PipewireOutputs outputs;
    if (!outputs.start()) {
        MESSAGE("no PipeWire to talk to; skipped");
        return;
    }
    REQUIRE(outputs.wait_ready());
    CHECK(outputs.running());

    const std::vector<PipewireSink> sinks = outputs.sinks();
    // The measurement rig declares these; anything else on the machine is extra.
    if (!has(sinks, "isotone_hw")) {
        MESSAGE("the rig's sinks are not declared here; skipped");
        return;
    }
    CHECK(has(sinks, "isotone_virt"));

    for (const PipewireSink& s : sinks) {
        CHECK_FALSE(s.name.empty());
        CHECK_FALSE(s.description.empty());   // falls back to the name
        CHECK(s.is_isotone == (s.name == "isotone"));
    }
}

TEST_CASE("the default sink is one of the sinks, when something has said") {
    PipewireOutputs outputs;
    if (!outputs.start()) {
        MESSAGE("no PipeWire to talk to; skipped");
        return;
    }
    REQUIRE(outputs.wait_ready());

    const std::string def = outputs.default_sink();
    if (def.empty()) {
        MESSAGE("no default has been set; skipped");
        return;
    }
    CHECK(has(outputs.sinks(), def));
}

TEST_CASE("the default is known once the outputs are ready, not a moment later") {
    // The metadata that holds it is bound during the registry sweep, and its
    // properties arrive after that sweep has been answered. A default that shows
    // up only after wait_ready() reads to the app as the default output changing
    // at startup, and "Switch preset when the default output changes" then moves
    // the session off the output it was opened on.
    for (int run = 0; run < 5; ++run) {
        PipewireOutputs outputs;
        if (!outputs.start()) {
            MESSAGE("no PipeWire to talk to; skipped");
            return;
        }
        REQUIRE(outputs.wait_ready());
        const std::string at_ready = outputs.default_sink();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::string later = outputs.default_sink();
        if (later.empty()) {
            MESSAGE("no default has been set; skipped");
            return;
        }
        CHECK(at_ready == later);
    }
}

TEST_CASE("stopping releases everything, and starting again works") {
    PipewireOutputs outputs;
    if (!outputs.start()) {
        MESSAGE("no PipeWire to talk to; skipped");
        return;
    }
    REQUIRE(outputs.wait_ready());
    const size_t before = outputs.sinks().size();

    outputs.stop();
    CHECK_FALSE(outputs.running());
    CHECK(outputs.sinks().empty());

    REQUIRE(outputs.start());
    REQUIRE(outputs.wait_ready());
    CHECK(outputs.sinks().size() == before);
}
