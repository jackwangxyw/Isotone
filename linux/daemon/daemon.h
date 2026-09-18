// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Linux host: stage 3's daemon, the counterpart of IsoAPO on Windows.
//
// It owns a virtual sink that applications play into, captures that sink's
// monitor, runs isotone_core's Processor over it, and plays the result into a
// hardware sink. The topology is the EasyEffects model measured in stage 1c
// (decisions.md, "Stage 1c complete, the PipeWire topology measured").
//
// Everything is created by the daemon itself through PipeWire's adapter and
// link-factory factories: no configuration file drops a sink in, and no session
// manager has to understand a filter. That matters for reach, because
// WirePlumber only grew smart-filter placement in 0.5 and Ubuntu 24.04, which is
// what Linux Mint 22 is built on, still ships 0.4.17.
//
// The edited state arrives through the same shared ParamBlock the Windows side
// uses, here a POSIX shared-memory object (linux/transport). The sink's saved
// state seeds that block, so a sink is already correct before any UI runs.

#pragma once

#include <cstdint>
#include <string>

namespace isotone::daemon {

struct Options {
    // node.name of the hardware sink the processed audio is played into. The
    // shared region and the saved-state file are named for this sink, because
    // it is the device the state belongs to, exactly as the endpoint GUID names
    // them on Windows.
    std::string target_sink;

    // The virtual sink applications see.
    std::string sink_name = "isotone";
    std::string sink_description = "Isotone";

    // Empty: persisted_state_dir(). Tests pass a scratch directory.
    std::string state_dir;

    // Frames the processor is sized for. A quantum above this is passed through
    // untouched for one cycle while the main loop resizes, rather than
    // allocating on the real-time thread.
    uint32_t max_frames = 8192;

    // Removes the shared region's name on a clean exit, so the next run starts
    // from the saved state rather than from a region the last run left behind.
    bool unlink_on_exit = true;

    // Exit once the graph is linked and one block has been processed. The
    // measurement harness uses this; a service never does.
    bool exit_when_linked = false;
};

// Runs until SIGINT or SIGTERM. Returns a process exit code.
int run(const Options& options);

}  // namespace isotone::daemon
