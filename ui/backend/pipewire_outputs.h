// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The outputs a Linux machine has, and which one is default: what
// isotone::devices does with IMMDeviceEnumerator on Windows, done with
// PipeWire's registry. No Qt, so the model can be tested without one.
//
// A thread of its own runs the PipeWire loop, because the UI thread must not
// block on it and PipeWire's loop wants to own its thread. Everything a caller
// reads is a snapshot taken under a lock; the change callback runs on the
// PipeWire thread, so a UI hands it straight to its own loop rather than
// touching widgets in it.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace isotone::ui {

// Defined in the .cpp: the PipeWire loop, the registry listeners and the state
// they keep. At namespace scope rather than nested, so the C callbacks the
// registry wants can name it.
struct PipewireOutputsImpl;

struct PipewireSink {
    // node.name: the identity an OutputTarget carries and the shared region is
    // named for. Stable across restarts, unlike the object id.
    std::string name;
    // What a person is shown: sink_display_name().
    std::string description;
    // Isotone's own virtual sink, which is an output to play into but never one
    // to feed: its monitor is the core's input, so targeting it is a loop.
    bool is_isotone = false;
};

// node.nick ("HDMI 3", "Speaker + Headphones"), else node.description, else
// node.name. A card's outputs share the start of their descriptions, which is
// all the sidebar has room for.
std::string sink_display_name(const char* nick, const char* description, const std::string& name);

class PipewireOutputs {
public:
    PipewireOutputs();
    ~PipewireOutputs();
    PipewireOutputs(const PipewireOutputs&) = delete;
    PipewireOutputs& operator=(const PipewireOutputs&) = delete;

    // Connects and starts the loop. False when there is no PipeWire to talk to,
    // which is the ordinary state on a machine that runs PulseAudio or ALSA
    // alone, and is not an error worth a dialog.
    bool start();
    void stop();
    bool running() const;

    // The sinks now, in the order PipeWire reported them. A snapshot: it does
    // not change under the caller.
    std::vector<PipewireSink> sinks() const;

    // node.name of the default sink, empty when nothing has said. This is what
    // WirePlumber's "default" metadata holds, the same thing the daemon follows.
    std::string default_sink() const;

    // Called after any of the above changes, on the PipeWire thread. Set it
    // before start(); it is not called again after stop() returns.
    void set_on_changed(std::function<void()> on_changed);

    // Waits for the first registry sweep to finish, so a caller that wants to
    // list outputs once does not have to poll. False on timeout.
    bool wait_ready(int timeout_ms = 2000);

private:
    std::unique_ptr<PipewireOutputsImpl> impl_;
};

}  // namespace isotone::ui
