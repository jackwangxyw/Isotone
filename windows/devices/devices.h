// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Render endpoints as the UI's Devices table needs them: identity, state,
// default roles, the engine's mix format, and which EQ engine the registry puts
// on each. Read-only: nothing here changes a device, its format or the
// registry, and nothing opens an audio stream. The library's one write is
// set_speaker_layout (speaker_layout.h).
//
// COM: the caller initialises COM on the calling thread, in either apartment
// (the MMDevice API objects used here are free-threaded). Every function that
// takes or returns a device ID creates its own IMMDeviceEnumerator.
//
// Errors: every function returns an HRESULT or carries an error text; none
// throws.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace isotone::devices {

enum class SampleFormat { pcm, ieee_float, other };

// PKEY_AudioEngine_DeviceFormat: the shared-mode mix format the audio engine
// runs the endpoint at, the value `isotone-devicetool status` reports as
// channels, sample_rate and channel_mask.
struct DeviceFormat {
    bool present = false;          // false: `error` says why, and nothing below is meaningful
    std::string error;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;  // container size
    uint16_t valid_bits = 0;       // equals bits_per_sample unless the format is extensible
    SampleFormat sample_format = SampleFormat::other;
    uint32_t channel_mask = 0;
    // The format carried no mask (not extensible, or an extensible mask of 0),
    // so channel_mask is isotone::default_speaker_mask(channels), as upstream
    // Equalizer APO assumes; 0 for a count it has no default for.
    bool mask_defaulted = false;
};

// Parses the property's VT_BLOB bytes: a WAVEFORMATEX, or a
// WAVEFORMATEXTENSIBLE when the tag says so. A blob shorter than its header
// plus cbSize, an extensible tag with cbSize below 22, or zero channels or rate
// is reported, not guessed at. Bytes past cbSize are ignored.
DeviceFormat parse_device_format(const uint8_t* data, size_t size);

// Reads the property from an endpoint's property store (IMMDevice ID).
DeviceFormat read_device_format(const std::wstring& device_id);

// Which EQ engine the registry puts on an endpoint, decided exactly as
// isotone-devicetool's list and status decide it (windows/devicetool/main.cpp):
// a CLSID of IsoAPO's or Equalizer APO's pre- or post-mix class in an effect
// slot (LFX GFX SFX MFX EFX, REG_SZ) or in an effect list (,13 ,14 ,15 ,19 ,20,
// REG_MULTI_SZ); install records under HKLM\SOFTWARE\IsoAPO\Child APOs\{guid}
// and HKLM\SOFTWARE\EqualizerAPO\Child APOs\{guid}; and devicetool's journal,
// HKLM\SOFTWARE\IsoAPO\Pending\{guid}.

// devicetool's "backend": the effect slots and lists alone.
enum class Backend {
    none,
    native,          // IsoAPO in a slot
    equalizerapo,    // Equalizer APO in a slot, no IsoAPO
    conflict,        // both in slots
};

// devicetool status's "isoapo.state", first match wins.
enum class IsoApoState {
    interrupted,                // devicetool's journal is there: an install, uninstall or repair was cut short
    unrecorded,                 // IsoAPO in a slot, no IsoAPO record (the retired install.ps1)
    alongside_equalizerapo,     // IsoAPO and Equalizer APO in slots
    installed,                  // IsoAPO in a slot
    not_installed,              // IsoAPO in no slot, no IsoAPO record
    replaced_by_equalizerapo,   // IsoAPO's record, and Equalizer APO's record names an IsoAPO class
    detached,                   // IsoAPO's record, IsoAPO in no slot (a driver update removed it)
};

struct EngineInfo {
    Backend backend = Backend::none;
    IsoApoState isoapo_state = IsoApoState::not_installed;
    // What both are decided from.
    bool isoapo_in_slots = false;
    bool equalizerapo_in_slots = false;
    bool isoapo_record = false;              // the key, or upstream's legacy REG_SZ value named by the GUID
    // Equalizer APO's record key holds an IsoAPO class as a slot's original
    // (REG_SZ, the five slot value names) or as PreMixChild or PostMixChild:
    // its Device Selector was ticked after IsoAPO was installed. devicetool's
    // "equalizerapo_over_isoapo".
    bool equalizerapo_over_isoapo = false;
    bool journal = false;                    // devicetool's "interrupted_command" is not null
    // FxProperties {1da5d803-...},5 is nonzero: Windows loads no APO on the endpoint.
    bool enhancements_disabled = false;
    // E_INVALIDARG: not a GUID or device ID. HRESULT_FROM_WIN32(ERROR_NOT_FOUND):
    // no such render endpoint in the registry. Otherwise a registry error. On
    // an error nothing above is meaningful.
    HRESULT error = S_OK;
};

// The decisions, from the fact fields of `facts` (backend, isoapo_state,
// enhancements_disabled and error are ignored).
Backend classify_backend(const EngineInfo& facts);
IsoApoState classify_isoapo_state(const EngineInfo& facts);

// devicetool's spelling of each: "native", "alongside_equalizerapo".
const char* backend_name(Backend backend);
const char* isoapo_state_name(IsoApoState state);

// Registry reads only, a few milliseconds. MMDevice notifications do not report
// engine changes (devicetool install, uninstall and repair write the registry
// only), so call this again after running devicetool, or on a timer.
// `endpoint` is anything isotone::win::canonical_endpoint_guid accepts.
EngineInfo read_engine(const std::wstring& endpoint);

// DEVICE_STATE_* bits as IMMDevice::GetState reports them.
struct Endpoint {
    std::wstring id;               // IMMDevice::GetId
    std::wstring guid;             // {lower-case}, the MMDevices key and the shared region's name
    DWORD state = 0;
    std::wstring friendly_name;    // PKEY_Device_FriendlyName, "CABLE Input (VB-Audio Virtual Cable)"
    std::wstring device_name;      // the adapter, {b3f8fa53-0004-438e-9003-51a46e139bfc},6; devicetool's "name"
    std::wstring connection_name;  // PKEY_Device_DeviceDesc; devicetool's "connection"
    bool default_console = false;
    bool default_multimedia = false;
    DeviceFormat format;
    EngineInfo engine;
    std::string error;             // a property store or ID read that failed; the fields it covers are empty
};

// Every render endpoint in every state (active, disabled, not present,
// unplugged). A failure on one endpoint is reported in its `error`; the
// HRESULT is for failures that stop the enumeration.
HRESULT enumerate_render_endpoints(std::vector<Endpoint>* out);

// One render endpoint, by device ID or anything isotone::win::canonical_endpoint_guid accepts.
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND) if there is no such render endpoint.
HRESULT read_render_endpoint(const std::wstring& endpoint, Endpoint* out);

}  // namespace isotone::devices
