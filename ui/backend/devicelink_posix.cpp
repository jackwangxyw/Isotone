// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Where an output's edits go on Linux. Much shorter than the Windows half,
// because there is one engine rather than two: the daemon owns the sink's shared
// region, and every edit is a seqlock write into it. There is no Equalizer APO to
// rewrite on a worker thread, and no loopback capture, because the daemon writes
// the post-EQ audio into the region's ring itself.
//
// The region exists only while a daemon is hosting that sink, so a write that
// finds none opens it again, exactly as the Windows side does when the engine is
// not running.

#include <chrono>
#include <fstream>

#include "devicelink.h"
#include "isotone/apo_config.h"
#include "persisted_state.h"

namespace isotone::ui {

namespace {

// How long to wait before trying the region again after a miss, so a sink with
// no daemon does not cost an open on every edit of a drag.
constexpr uint64_t kReopenAfterMs = 500;

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

DeviceLink::DeviceLink(std::string state_dir) : state_dir_(std::move(state_dir)) {}

DeviceLink::~DeviceLink() = default;

void DeviceLink::set_target(const OutputTarget& target) {
    if (target.guid == target_.guid && target.backend == target_.backend) {
        target_ = target;   // the layout may have moved
        return;
    }
    region_.close();
    region_opened_ = false;
    last_open_attempt_ms_ = 0;
    ring_cursor_ = AudioRingCursor{};
    target_ = target;
    ensure_region();
}

bool DeviceLink::ensure_region() {
    if (region_.is_open()) return true;
    if (target_.guid.empty() || target_.backend != Backend::pipewire) return false;

    const uint64_t now = now_ms();
    if (last_open_attempt_ms_ != 0 && now - last_open_attempt_ms_ < kReopenAfterMs) return false;
    last_open_attempt_ms_ = now;

    if (region_.open(posix::region_name(target_.guid)) != 0) return false;
    region_opened_ = true;
    return true;
}

LinkError DeviceLink::write_region(const EqState& engine_state) {
    if (!ensure_region()) return static_cast<LinkError>(ENOENT);
    ParamBlock wanted{};
    init_param_block(&wanted);
    if (!to_param_block(engine_state, &wanted)) return static_cast<LinkError>(EINVAL);

    // The parameters only: the header is the daemon's, and holds the format and
    // the heartbeat this side reads back.
    param_block_write(region_.params(), [&wanted](ParamBlock* live) {
        const ParamBlockHeader header = live->hdr;
        *live = wanted;
        live->hdr = header;
    });
    return kLinkOk;
}

LinkError DeviceLink::apply(const EqState& state) { return write_region(state); }

LinkError DeviceLink::commit(const EqState& state) { return write_region(state); }

LinkError DeviceLink::save(const EqState& state, const EqState& engine_state) {
    // The file first, then the region, as the contract asks: a daemon that starts
    // between the two must not come up with the older state.
    ParamBlock saved{};
    init_param_block(&saved);
    if (!to_param_block(state, &saved)) return static_cast<LinkError>(EINVAL);

    const std::filesystem::path path = saved_state_path();
    if (path.empty()) return static_cast<LinkError>(EINVAL);
    if (const int error = posix::write_persisted_state(path.string(), saved); error != 0) {
        return static_cast<LinkError>(error);
    }
    const LinkError region = write_region(engine_state);
    // No daemon is not a failure to save: the file is what it will start from.
    return region == static_cast<LinkError>(ENOENT) ? kLinkOk : region;
}

std::filesystem::path DeviceLink::saved_state_path() const {
    const std::string dir = state_dir_.empty() ? posix::persisted_state_dir() : state_dir_;
    const std::string path = posix::persisted_state_path(dir, target_.guid);
    return path.empty() ? std::filesystem::path() : std::filesystem::path(path);
}

bool DeviceLink::load_current(EqState* out) {
    if (out == nullptr) return false;

    // What the sink plays now: the daemon's region, or the saved state when no
    // daemon is hosting it.
    if (ensure_region()) {
        ParamBlock block{};
        if (param_block_read(region_.params(), &block)) {
            from_param_block(block, out);
            return true;
        }
    }
    ParamBlock saved{};
    const std::filesystem::path path = saved_state_path();
    if (!path.empty() &&
        posix::read_persisted_state(path.string(), &saved) == posix::PersistedRead::Loaded) {
        from_param_block(saved, out);
        return true;
    }
    return false;
}

uint32_t DeviceLink::read_audio(float* out, uint32_t max_frames, uint32_t* channels,
                                double* sample_rate) {
    if (out == nullptr || channels == nullptr) return 0;
    if (!ensure_region()) return 0;
    if (sample_rate != nullptr) {
        const uint32_t rate = region_.params()->hdr.sample_rate;
        if (rate != 0) *sample_rate = rate;
    }
    return audio_ring_read(region_.ring(), kRingCapacityFrames, &ring_cursor_, out, max_frames,
                           channels);
}

bool DeviceLink::region_open() const { return region_.is_open(); }

// There is no second backend on this platform, so nothing here is asynchronous
// and there is never a deferred error to report.
LinkError DeviceLink::last_compat_error() const { return kLinkOk; }

uint64_t DeviceLink::compat_writes() const { return 0; }

}  // namespace isotone::ui
