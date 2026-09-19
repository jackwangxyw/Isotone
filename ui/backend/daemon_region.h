// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// What the Linux daemon says about the sink it feeds, read from that sink's
// region header: the format the core runs at and the heartbeat. The region
// exists only while a daemon hosts the sink (linux/daemon removes its name when
// it lets the sink go), so finding one is how the UI tells which output is fed.
// The counterpart of windows/devices' engine probe, without the probe.

#pragma once

#include <cstdint>
#include <string>

namespace isotone::ui {

struct DaemonRegion {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t speaker_mask = 0;
    uint32_t heartbeat = 0;
};

// The header of `node_name`'s region, or an errno: ENOENT when no daemon feeds
// that sink, EPROTO when what is there is not a region this build understands.
int read_daemon_region(const std::string& node_name, DaemonRegion* out);

}  // namespace isotone::ui
