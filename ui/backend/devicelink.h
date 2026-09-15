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

#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "isotone/audio_ring.h"
#include "isotone/types.h"
#include "output_state.h"
#include "shared_mapping.h"

namespace isotone::compat {
class CompatWriter;
class LoopbackCapture;
}  // namespace isotone::compat

namespace isotone::ui {

enum class Backend { none, native, equalizer_apo };

struct OutputTarget {
    std::wstring guid;        // {lower-case}
    Backend backend = Backend::none;
    OutputLayout layout;
};

class DeviceLink {
public:
    // `region_namespace` precedes "IsoAPO.{guid}": Global\ for IsoAPO, a Local\
    // prefix in tests. `compat_config_dir` empty: Equalizer APO's own config
    // directory, from its install; tests pass a sandbox.
    explicit DeviceLink(std::wstring region_namespace = L"Global\\", std::wstring compat_config_dir = L"");
    DeviceLink(const DeviceLink&) = delete;
    DeviceLink& operator=(const DeviceLink&) = delete;
    // Flushes a pending Equalizer APO write.
    ~DeviceLink();

    void set_target(const OutputTarget& target);
    const OutputTarget& target() const { return target_; }

    // A live edit (a drag in progress). Win32 error code; for native,
    // ERROR_FILE_NOT_FOUND means the engine is idle and nothing was written.
    DWORD apply(const EqState& state);
    // An edit that is done (release, a toggle, a typed value).
    DWORD commit(const EqState& state);
    // `state` to the saved state file, and `engine_state` (the same state with
    // what is only live, such as a solo) to the region when there is one: the
    // file first, then the region, as the contract asks. Native only.
    DWORD save(const EqState& state, const EqState& engine_state);
    // The target's saved state file: the self test's directory unless the region
    // namespace is Global\.
    std::wstring saved_state_path() const;

    // What the output plays now: native, the region, or the saved state when the
    // engine is idle; Equalizer APO, its block in Isotone.txt. False when there is
    // none (the output plays flat).
    bool load_current(EqState* out);

    // The spectrum's audio since the last call, interleaved; `channels` gets the
    // samples per frame. 0 frames when there is no source.
    uint32_t read_audio(float* out, uint32_t max_frames, uint32_t* channels, double* sample_rate);

    bool region_open() const { return mapping_.is_open(); }
    // True once after the region was found again (an engine started playing):
    // what was edited while it was idle has not reached it yet.
    bool take_region_opened() { return std::exchange(region_opened_, false); }
    // The last Equalizer APO write's result (asynchronous).
    DWORD last_compat_error() const;

private:
    DWORD write_region(const EqState& engine_state);
    bool ensure_region();
    void compat_thread();
    void stop_compat();
    void start_capture();

    std::wstring namespace_;
    std::wstring compat_dir_;
    OutputTarget target_;

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
    DWORD compat_error_ = ERROR_SUCCESS;

    std::unique_ptr<isotone::compat::LoopbackCapture> capture_;
    AudioRingCursor capture_cursor_;
};

}  // namespace isotone::ui
