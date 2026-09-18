// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Where an output's edits go (docs/ui-spec.md, "Framework" and "Engine
// contracts"):
//   native (IsoAPO)  the endpoint's shared region, every edit, through the
//                    seqlock; the region exists only while the engine runs, so
//                    it is opened again when a write finds none
//   Equalizer APO    CompatWriter on a worker thread: apply during a drag writes
//                    nothing, commit persists once (every write reloads every
//                    Equalizer APO device from rest)
// save() writes the per-endpoint saved state IsoAPO starts with; the UI calls it
// when a preset is saved or assigned.
//
// The spectrum's audio: the region's ring (native) or WASAPI loopback on the
// endpoint (Equalizer APO), read through read_audio().
//
// Not thread-safe: one owner thread (the UI thread) calls everything.

#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "isotone/audio_ring.h"
#include "isotone/types.h"
#include "output_state.h"
#if defined(_WIN32)
#include "shared_mapping.h"
#else
#include "shared_region.h"
#endif

namespace isotone::compat {
class CompatWriter;
class LoopbackCapture;
}  // namespace isotone::compat

namespace isotone::ui {

// What an output's edits reach. native and equalizer_apo are Windows engines;
// pipewire is the Linux daemon, which hosts the same core behind the same shared
// ParamBlock and audio ring, so everything above this layer is unchanged.
enum class Backend { none, native, equalizer_apo, pipewire };

// A platform error code, 0 for success: a Win32 code on Windows, an errno on
// Linux. Callers above this layer only ever compare it with kLinkOk or show it.
using LinkError = uint32_t;
inline constexpr LinkError kLinkOk = 0;

struct OutputTarget {
    // How the platform names the output this state belongs to: an endpoint GUID
    // as "{lower-case}" on Windows, a PipeWire node.name on Linux. Narrow rather
    // than wide because it is the one thing every backend has to agree on, and
    // the Windows one widens it at its own edge; a canonical GUID is ASCII, so
    // nothing is lost.
    std::string guid;
    Backend backend = Backend::none;
    OutputLayout layout;
};

// The identity as the Win32 calls want it, and back. Only the Windows backend
// needs these.
std::wstring widen_id(const std::string& id);
std::string narrow_id(const std::wstring& id);

class DeviceLink {
public:
    // `region_namespace` precedes "IsoAPO.{guid}": the Global namespace for
    // IsoAPO, a Local one in tests. Named rather than written out: a comment
    // line ending in a backslash continues onto the next one.
    // `compat_config_dir` empty: Equalizer APO's own config
    // directory, from its install; tests pass a sandbox.
    // `retry_for_ms`: how long a failed Equalizer APO write keeps being retried
    // (tests shorten it).
#if defined(_WIN32)
    explicit DeviceLink(std::wstring region_namespace = L"Global\\", std::wstring compat_config_dir = L"",
                        int retry_for_ms = 10000);
#else
    // Linux: the daemon owns the region, so there is no namespace to choose and
    // no Equalizer APO to write. `state_dir` empty is the daemon's own,
    // $XDG_CONFIG_HOME/isotone/devices; tests pass a scratch directory.
    explicit DeviceLink(std::string state_dir = {});
#endif
    DeviceLink(const DeviceLink&) = delete;
    DeviceLink& operator=(const DeviceLink&) = delete;
    // Flushes a pending Equalizer APO write.
    ~DeviceLink();

    void set_target(const OutputTarget& target);
    const OutputTarget& target() const { return target_; }

    // A live edit (a drag in progress). Win32 error code; for native,
    // ERROR_FILE_NOT_FOUND means the engine is idle and nothing was written.
    LinkError apply(const EqState& state);
    // An edit that is done (release, a toggle, a typed value).
    LinkError commit(const EqState& state);
    // `state` to the saved state file, and `engine_state` (the same state with
    // what is only live, such as a solo) to the region when there is one: the
    // file first, then the region, as the contract asks. Native only.
    LinkError save(const EqState& state, const EqState& engine_state);
    // The target's saved state file: the self test's directory unless the region
    // namespace is Global\.
    std::filesystem::path saved_state_path() const;

    // What the output plays now: native, the region, or the saved state when the
    // engine is idle; Equalizer APO, its block in Isotone.txt. False when there is
    // none (the output plays flat).
    bool load_current(EqState* out);

    // The spectrum's audio since the last call, interleaved; `channels` gets the
    // samples per frame. 0 frames when there is no source.
    uint32_t read_audio(float* out, uint32_t max_frames, uint32_t* channels, double* sample_rate);

    // True while the engine's shared region is open.
    bool region_open() const;
    // True once after the region was found again (an engine started playing):
    // what was edited while it was idle has not reached it yet.
    bool take_region_opened() { return std::exchange(region_opened_, false); }
    // The last Equalizer APO write's result (asynchronous).
    LinkError last_compat_error() const;
    // How many Equalizer APO writes the worker has made, retries included: a
    // retry that a newer edit replaces costs no write of its own.
    uint64_t compat_writes() const;

private:
    LinkError write_region(const EqState& engine_state);
    bool ensure_region();
    void compat_thread();
    void stop_compat();
    void start_capture();

    OutputTarget target_;

#if defined(_WIN32)
    std::wstring namespace_;
    std::wstring compat_dir_;
    int compat_retry_for_ms_ = 10000;

    isotone::win::SharedMapping mapping_;
    ULONGLONG last_open_attempt_ = 0;
    bool region_opened_ = false;
    AudioRingCursor ring_cursor_;

    // Equalizer APO: the newest request, taken by the worker.
    struct CompatRequest {
        EqState state;
        OutputTarget target;
        bool persist = false;
    };
    std::thread compat_worker_;
    mutable std::mutex compat_mutex_;
    std::condition_variable compat_wake_;
    std::optional<CompatRequest> compat_pending_;
    bool compat_stop_ = false;
    LinkError compat_error_ = kLinkOk;
    uint64_t compat_writes_ = 0;

    std::unique_ptr<isotone::compat::LoopbackCapture> capture_;
    AudioRingCursor capture_cursor_;
#else
    // Linux: the daemon is the only writer of the region and there is no second
    // backend, so none of the Equalizer APO worker or the loopback capture has a
    // counterpart here. The region is the daemon's, opened read/write.
    std::string         state_dir_;
    posix::SharedRegion region_;
    bool                region_opened_ = false;
    uint64_t            last_open_attempt_ms_ = 0;
    AudioRingCursor     ring_cursor_;
#endif
};

}  // namespace isotone::ui
