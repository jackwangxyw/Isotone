// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Whether IsoAPO is processing an endpoint right now, from its shared region
// (windows/transport) and the endpoint's audio sessions.
//
// IsoAPO creates Global\IsoAPO.{guid} when audiodg first initialises it and
// increments host_heartbeat on every process call (param_block.h). audiodg only
// calls APOProcess while a stream on the endpoint runs. host_state is left as
// it was when an instance goes away, so liveness is the heartbeat moving, never
// host_state. A region outlives IsoAPO while any process (the UI) holds it.
//
// The states the UI can tell apart (EngineActivity):
//   not_installed      IsoAPO in no effect slot (read_engine), heartbeat still
//   idle               installed; no region, or a still heartbeat, and no active session
//   running            the heartbeat moved; the region's format is what IsoAPO runs
//   stalled            installed, a session is active, the heartbeat is still or
//                      there is no region: the only sign that IsoAPO failed to load
//                      or is not processing
//   region_unreadable  the region exists but cannot be read: another IsoAPO
//                      build's layout (ERROR_INVALID_DATA) or access denied
//   unknown            installed, heartbeat still, and the session query failed
//
// "Active session" is AudioSessionStateActive: "At least one of the streams in
// the session is running" (learn.microsoft.com/en-us/windows/win32/api/
// audiosessiontypes/ne-audiosessiontypes-audiosessionstate), counted through
// IAudioSessionManager2::GetSessionEnumerator; no stream is opened. Known ways
// `stalled` can be wrong, none measured:
//   - the enumerator "might not be aware of the new sessions" (GetSessionEnumerator
//     remarks), so a session started moments ago can be missed (reads idle);
//   - a session that is active on a stream the engine does not route through
//     IsoAPO's slot (an exclusive-mode stream, a raw-mode stream, or IsoAPO in a
//     slot the stream's processing mode skips) reads stalled while IsoAPO is fine;
//   - enhancements disabled on the endpoint (EngineInfo::enhancements_disabled)
//     also reads stalled, correctly: nothing processes.
//
// COM: initialised by the caller, either apartment.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "devices.h"

namespace isotone::devices {

inline constexpr wchar_t kRegionNamespace[] = L"Global\\";
inline constexpr DWORD kHeartbeatIntervalMs = 200;   // as isotone-shm status waits

struct RegionSample {
    // ERROR_SUCCESS, ERROR_FILE_NOT_FOUND (no region), ERROR_INVALID_DATA (a
    // layout this build does not understand), or another Win32 error.
    DWORD open_error = ERROR_FILE_NOT_FOUND;
    uint32_t heartbeat = 0;
    // As the host last published them; stale when the heartbeat is still.
    uint32_t version = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t speaker_mask = 0;
    uint32_t host_state = 0;   // isotone::HostState
};

// Opens the region through SharedMapping::open, which never creates one, reads
// the header and closes it. `name_prefix` precedes "IsoAPO.{guid}"; tests pass a
// unique Local\ prefix. Blocks at most the 50 ms SharedMapping::open waits for a
// region whose creator is still writing its header.
RegionSample sample_region(const std::wstring& endpoint, const wchar_t* name_prefix = kRegionNamespace);

bool heartbeat_moved(const RegionSample& earlier, const RegionSample& later);

// Sessions on the render endpoint in AudioSessionStateActive. `endpoint` is a
// device ID or anything isotone::win::canonical_endpoint_guid accepts.
HRESULT count_active_sessions(const std::wstring& endpoint, int* active);

enum class EngineActivity { not_installed, idle, running, stalled, region_unreadable, unknown };

// The classification above, as a pure function. `active_sessions` < 0 means
// the session query failed.
EngineActivity classify_activity(bool isoapo_in_slots, const RegionSample& earlier, const RegionSample& later,
                                 int active_sessions);

struct EngineProbe {
    EngineActivity activity = EngineActivity::unknown;
    EngineInfo engine;
    RegionSample earlier;
    RegionSample later;           // its format fields are the running format when activity is running
    int active_sessions = -1;
    HRESULT sessions_error = S_OK;
};

// read_engine, two region samples `interval_ms` apart, and the session count.
// Blocks for interval_ms, plus up to 50 ms per region open, plus the session
// query (a cross-process call into the audio service, not bounded here). Run it
// off the UI thread. Returns the first error that prevents a classification
// (read_engine's), otherwise S_OK with any session error in sessions_error.
HRESULT probe_engine(const std::wstring& endpoint, EngineProbe* out, DWORD interval_ms = kHeartbeatIntervalMs,
                     const wchar_t* name_prefix = kRegionNamespace);

}  // namespace isotone::devices
