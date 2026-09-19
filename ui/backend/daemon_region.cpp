// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "daemon_region.h"

#include <cerrno>

#include "isotone/param_block.h"
#include "shared_region.h"

namespace isotone::ui {

int read_daemon_region(const std::string& node_name, DaemonRegion* out) {
    if (out == nullptr) return EINVAL;
    posix::SharedRegion region;
    if (const int error = region.open(posix::region_name(node_name)); error != 0) return error;
    const ParamBlockHeader& hdr = region.params()->hdr;
    if (hdr.magic != kParamMagic || hdr.version != kParamVersion) return EPROTO;
    // Header fields the host writes one at a time: each is read once, so a
    // torn pair is at worst a stale value for one probe, never a crash.
    out->sample_rate = __atomic_load_n(&hdr.sample_rate, __ATOMIC_ACQUIRE);
    out->channels = __atomic_load_n(&hdr.channels, __ATOMIC_ACQUIRE);
    out->speaker_mask = __atomic_load_n(&hdr.speaker_mask, __ATOMIC_ACQUIRE);
    out->heartbeat = __atomic_load_n(&hdr.host_heartbeat, __ATOMIC_ACQUIRE);
    return 0;
}

}  // namespace isotone::ui
