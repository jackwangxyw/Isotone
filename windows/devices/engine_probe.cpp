// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "engine_probe.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include "shared_mapping.h"

namespace isotone::devices {

RegionSample sample_region(const std::wstring& endpoint, const wchar_t* name_prefix) {
    RegionSample s;
    const std::wstring guid = isotone::win::canonical_endpoint_guid(endpoint);
    if (guid.empty()) {
        s.open_error = ERROR_INVALID_PARAMETER;
        return s;
    }
    isotone::win::SharedMapping mapping;
    s.open_error = mapping.open(isotone::win::mapping_name(name_prefix, guid));
    if (s.open_error != ERROR_SUCCESS) return s;
    const ParamBlockHeader& hdr = mapping.params()->hdr;
    s.heartbeat = hdr.host_heartbeat;
    s.version = hdr.version;
    s.sample_rate = hdr.sample_rate;
    s.channels = hdr.channels;
    s.speaker_mask = hdr.speaker_mask;
    s.host_state = hdr.host_state;
    return s;
}

bool heartbeat_moved(const RegionSample& earlier, const RegionSample& later) {
    return earlier.open_error == ERROR_SUCCESS && later.open_error == ERROR_SUCCESS &&
           earlier.heartbeat != later.heartbeat;
}

HRESULT count_active_sessions(const std::wstring& endpoint, int* active) {
    *active = -1;
    Endpoint e;
    HRESULT hr = read_render_endpoint(endpoint, &e);
    if (FAILED(hr)) return hr;
    if (e.state != DEVICE_STATE_ACTIVE) {
        // Only an active endpoint can run a stream, and it cannot be activated.
        *active = 0;
        return S_OK;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return hr;
    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(e.id.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr)) return hr;
    IAudioSessionManager2* manager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager));
    device->Release();
    if (FAILED(hr)) return hr;
    IAudioSessionEnumerator* sessions = nullptr;
    hr = manager->GetSessionEnumerator(&sessions);
    manager->Release();
    if (FAILED(hr)) return hr;

    int count = 0;
    int running = 0;
    hr = sessions->GetCount(&count);
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        hr = sessions->GetSession(i, &control);
        if (FAILED(hr)) break;
        AudioSessionState state = AudioSessionStateInactive;
        hr = control->GetState(&state);
        control->Release();
        if (SUCCEEDED(hr) && state == AudioSessionStateActive) ++running;
    }
    sessions->Release();
    if (FAILED(hr)) return hr;
    *active = running;
    return S_OK;
}

EngineActivity classify_activity(bool isoapo_in_slots, const RegionSample& earlier, const RegionSample& later,
                                 int active_sessions) {
    if (heartbeat_moved(earlier, later)) return EngineActivity::running;
    if (!isoapo_in_slots) return EngineActivity::not_installed;
    const auto unreadable = [](const RegionSample& s) {
        return s.open_error != ERROR_SUCCESS && s.open_error != ERROR_FILE_NOT_FOUND;
    };
    if (unreadable(earlier) || unreadable(later)) return EngineActivity::region_unreadable;
    if (active_sessions < 0) return EngineActivity::unknown;
    return active_sessions > 0 ? EngineActivity::stalled : EngineActivity::idle;
}

HRESULT probe_engine(const std::wstring& endpoint, EngineProbe* out, DWORD interval_ms, const wchar_t* name_prefix) {
    *out = EngineProbe{};
    out->engine = read_engine(endpoint);
    if (FAILED(out->engine.error)) return out->engine.error;
    out->earlier = sample_region(endpoint, name_prefix);
    Sleep(interval_ms);
    out->later = sample_region(endpoint, name_prefix);
    out->sessions_error = count_active_sessions(endpoint, &out->active_sessions);
    out->activity = classify_activity(out->engine.isoapo_in_slots, out->earlier, out->later, out->active_sessions);
    return S_OK;
}

}  // namespace isotone::devices
