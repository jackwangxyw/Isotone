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
