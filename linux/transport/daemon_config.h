// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The daemon's settings that outlive a run, shared by the daemon and the UI:
// the virtual sink's channel count, which the Speakers view's layout picker
// chooses. On Windows the layout is the endpoint's own format; here it is the
// virtual sink's, which the daemon creates, so it is kept where the daemon
// reads it at startup and a change takes a restart of the user service.
//
// $XDG_CONFIG_HOME/isotone/daemon.conf, one "key=value" per line:
//   channels=6

#pragma once

#include <cstdint>
#include <string>

namespace isotone::posix {

// The channel counts the daemon lays out: mono, stereo, 2.1, quad, 5.1, 7.1.
bool daemon_channels_supported(uint32_t channels);

// $XDG_CONFIG_HOME/isotone/daemon.conf, or under $HOME/.config when
// XDG_CONFIG_HOME is unset or not absolute. Empty when neither is usable.
std::string daemon_config_path();

// The channel count the file holds, 0 when it holds none the daemon supports
// (no file, unreadable, no such key, or a count it does not lay out).
uint32_t read_daemon_channels(const std::string& path);

// Writes the channel count, keeping any other line, atomically (a temporary file
// beside it, then a rename). Creates the directory. An errno value, 0 on
// success; EINVAL for a count the daemon does not lay out.
int write_daemon_channels(const std::string& path, uint32_t channels);

}  // namespace isotone::posix
