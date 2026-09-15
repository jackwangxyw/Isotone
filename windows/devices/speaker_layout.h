// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Speakers view's layout picker: which of Stereo, 2.1, 5.1 and 7.1 an output
// supports, and changing the output's Windows speaker setup to one of them. The
// one write in isotone_devices: set_speaker_layout changes the endpoint's device
// format (channel count and mask) the way the Sound control panel's Default
// Format does. Everything else here only reads.
//
// Endpoint identifiers are a device ID or anything
// isotone::win::canonical_endpoint_guid accepts.
//
// COM: initialised by the caller, either apartment.
//
// Errors: every function returns an HRESULT; none throws.
//
// IPolicyConfig. Undocumented; CLSID PolicyConfigClient
// {870af99c-171d-4f9e-af0d-e63df40c2bc9}, IID {f8679f50-850a-41cf-9c72-430f290290c8},
// twelve methods after IUnknown in this order:
//   0 GetMixFormat(PCWSTR, WAVEFORMATEX**)
//   1 GetDeviceFormat(PCWSTR, INT bDefault, WAVEFORMATEX**)
//   2 ResetDeviceFormat(PCWSTR)
//   3 SetDeviceFormat(PCWSTR, WAVEFORMATEX* endpoint, WAVEFORMATEX* mix)
//   4 GetProcessingPeriod, 5 SetProcessingPeriod, 6 GetShareMode, 7 SetShareMode,
//   8 GetPropertyValue, 9 SetPropertyValue, 10 SetDefaultEndpoint, 11 SetEndpointVisibility
// Sources, read 2026-09-14:
//   - EreTIk's PolicyConfig.h ("@compatible: Windows 7 and Later"), the same text in
//     https://github.com/tartakynov/audioswitch/blob/master/IPolicyConfig.h,
//     https://github.com/DanStevens/AudioEndPointController/blob/master/EndPointController/PolicyConfig.h,
//     https://github.com/Belphemur/AudioEndPointLibrary/blob/master/DefSound/PolicyConfig.h
//     (SoundSwitch's author's library) and
//     https://github.com/LizardByte/Sunshine/blob/master/src/platform/windows/PolicyConfig.h
//     (Sunshine calls SetDeviceFormat through it on Windows 10 and 11): the IID,
//     CLSID and all twelve methods in this order.
//   - https://github.com/frgnca/AudioDeviceCmdlets/blob/master/SOURCE/IPolicyConfig.cs:
//     the same IID and order, names GetDeviceFormat's INT "bDefault".
//   - https://github.com/File-New-Project/EarTrumpet/blob/dev/EarTrumpet/Interop/MMDeviceAPI/IPolicyConfig.cs:
//     the same IID for "Win7-Win8, W10_RS1-Present", eight slots before
//     GetPropertyValue (so ResetDeviceFormat is there), and other IIDs on Windows 10
//     1507 (CA286FC3-...) and 1511 (6BE54BE8-...), where creating the interface fails
//     with E_NOINTERFACE and nothing is called.
// Methods 0, 1 and 3 are the ones used. On this machine (Windows 11 26200) the
// owner's scratch tool over them read the cable's format matching
// PKEY_AudioEngine_DeviceFormat byte for byte with bDefault 0, and set 7.1 and
// back (decisions.md, 2026-09-12 and 2026-09-14). The returned formats are freed
// with CoTaskMemFree, the COM rule for [out] allocations; no source shows it.
//
// Elevation. No source found says whether SetDeviceFormat needs an elevated
// process. Microsoft documents that IMMDevice::OpenPropertyStore gives a client
// that is not an administrator read-only access, but IPolicyConfig is not that
// path. The endpoint's Properties key grants BUILTIN\Users QueryValues and
// SetValue on this machine (read 2026-09-14), so the registry does not require
// it either. Unverified until run unelevated.
//
// What changes. SetDeviceFormat takes the endpoint (device) format and the
// shared-mode mix format; nothing else is set here. Windows' Configure Speakers
// setting, PKEY_AudioEndpoint_PhysicalSpeakers ({1da5d803-...},3), is not
// changed. It is absent on every active render endpoint of this machine, the
// cable included after it was set to 7.1 and back, and upstream Equalizer APO
// reads it only when the device format carries no mask (DeviceAPOInfo.cpp:220),
// which a format written here always does. Isotone itself never reads it.
// Streams open on the endpoint are expected to be invalidated by the change
// (not measured); DeviceWatcher reports format_changed, after which the UI
// re-reads the endpoint.

#pragma once

#include <windows.h>

#include <mmreg.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devices.h"

namespace isotone::devices {

enum class SpeakerLayout { stereo, two_point_one, five_point_one, seven_point_one };

struct SpeakerLayoutSpec {
    SpeakerLayout layout;
    uint16_t channels;
    uint32_t mask;        // ksmedia.h KSAUDIO_SPEAKER_*
    const char* name;     // "stereo", "2.1", "5.1", "7.1"
};

// In picker order; kSpeakerLayouts[i].layout == SpeakerLayout(i).
//   stereo  2  0x3    KSAUDIO_SPEAKER_STEREO            FL FR
//   2.1     3  0xB    KSAUDIO_SPEAKER_2POINT1           FL FR LFE
//   5.1     6  0x60F  KSAUDIO_SPEAKER_5POINT1_SURROUND  FL FR FC LFE SL SR
//   7.1     8  0x63F  KSAUDIO_SPEAKER_7POINT1_SURROUND  FL FR FC LFE BL BR SL SR
// 5.1 and 7.1 are the masks isotone::default_speaker_mask gives 6 and 8
// channels. It gives 3 channels no mask, so 2.1 exists only with its mask.
inline constexpr SpeakerLayoutSpec kSpeakerLayouts[] = {
    {SpeakerLayout::stereo, 2, 0x3, "stereo"},
    {SpeakerLayout::two_point_one, 3, 0xB, "2.1"},
    {SpeakerLayout::five_point_one, 6, 0x60F, "5.1"},
    {SpeakerLayout::seven_point_one, 8, 0x63F, "7.1"},
};

// Null for a value outside the enum.
const SpeakerLayoutSpec* speaker_layout_spec(SpeakerLayout layout);

// The spec's name, case-sensitive. False for anything else.
bool parse_speaker_layout(std::string_view name, SpeakerLayout* out);

// The layout whose channel count and mask the format has. False when it has
// none of them, or is not present. A defaulted mask counts (a 2-channel format
// without a mask is stereo, as upstream reads it).
bool current_speaker_layout(const DeviceFormat& format, SpeakerLayout* out);

struct LayoutFormats {
    WAVEFORMATEXTENSIBLE endpoint{};
    WAVEFORMATEXTENSIBLE mix{};
};

// The formats set_speaker_layout passes for `layout`, from the device's current
// format. Both are WAVEFORMATEXTENSIBLE (cbSize 22) with the layout's channels
// and mask, and block align and average bytes per second computed.
//   endpoint: the current sample rate, bit depth, valid bits and subformat (PCM
//             or IEEE float; a plain WAVEFORMATEX PCM or float tag maps to its
//             subformat).
//   mix:      32-bit IEEE float, 32 valid bits, at the current sample rate.
// E_POINTER: `out` is null. E_INVALIDARG: `layout` is not in the enum.
// HRESULT_FROM_WIN32(ERROR_INVALID_DATA): the current format cannot be the base
// (not present, another subformat, no bits or not whole bytes, valid bits 0 or
// above the container, or sizes that overflow the format's fields). `out` is
// written only on S_OK.
HRESULT build_layout_formats(const DeviceFormat& current, SpeakerLayout layout, LayoutFormats* out);

// The layouts the output supports, in kSpeakerLayouts order: each layout's
// endpoint format (build_layout_formats) passes
// IAudioClient::IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE). The current
// format, at its own channels and mask, is asked first: Windows chose it from
// the formats the driver supports, so a refusal of it means the check says
// nothing about this output.
//   S_OK                                    `out` holds the supported layouts (possibly none)
//   E_POINTER                               `out` is null
//   HRESULT_FROM_WIN32(ERROR_NOT_FOUND)     no such render endpoint
//   HRESULT_FROM_WIN32(ERROR_NOT_READY)     the endpoint is not active (disabled, unplugged, not present)
//   HRESULT_FROM_WIN32(ERROR_INVALID_DATA)  its current format cannot be the base (build_layout_formats)
//   kCurrentFormatRefused                   IsFormatSupported answered AUDCLNT_E_UNSUPPORTED_FORMAT
//                                           for the current format
//   any other failure                       from activating IAudioClient or from IsFormatSupported,
//                                           as returned, for the first format that failed:
//                                           AUDCLNT_E_DEVICE_INVALIDATED, AUDCLNT_E_SERVICE_NOT_RUNNING,
//                                           and whatever exclusive mode being disallowed on the output
//                                           returns (not measured: IsFormatSupported's page lists no
//                                           such code, Initialize's is AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
//                                           if it answers "not supported" instead, that is
//                                           kCurrentFormatRefused)
// Only AUDCLNT_E_UNSUPPORTED_FORMAT for a layout's format means "not supported".
// `out` is cleared first and holds nothing on a failure. No stream is opened.
HRESULT supported_speaker_layouts(const std::wstring& endpoint, std::vector<SpeakerLayout>* out);

// Interface-specific failures (FACILITY_ITF).
inline constexpr HRESULT kCurrentFormatRefused = static_cast<HRESULT>(0x80040201L);
// IPolicyConfig::GetDeviceFormat does not match PKEY_AudioEngine_DeviceFormat:
// the interface is not what the sources above describe on this Windows build,
// so SetDeviceFormat is not called.
inline constexpr HRESULT kPolicyConfigMismatch = static_cast<HRESULT>(0x80040202L);
// SetDeviceFormat succeeded but the formats read back are not the ones passed.
inline constexpr HRESULT kLayoutReadBackMismatch = static_cast<HRESULT>(0x80040203L);

struct LayoutChange {
    std::wstring device_id;         // IMMDevice::GetId
    DeviceFormat before;            // PKEY_AudioEngine_DeviceFormat before
    DeviceFormat policy_before;     // IPolicyConfig::GetDeviceFormat(bDefault 0) before
    DeviceFormat mix_before;        // IPolicyConfig::GetMixFormat before
    LayoutFormats requested;
    bool set_called = false;        // SetDeviceFormat was called (whatever it returned)
    DeviceFormat after;             // IPolicyConfig::GetDeviceFormat after
    DeviceFormat mix_after;         // IPolicyConfig::GetMixFormat after
    // PKEY_AudioEngine_DeviceFormat after, reported and not compared: whether
    // the property store has the new value as soon as SetDeviceFormat returns is
    // not measured.
    DeviceFormat property_after;
    std::string error;              // what failed, when the HRESULT is a failure
};

// Every check set_speaker_layout makes, and nothing else: resolves the endpoint,
// builds the formats, asks IsFormatSupported(EXCLUSIVE) for the layout's endpoint
// format, creates IPolicyConfig and compares its GetDeviceFormat with the
// property. Fills `out` up to requested and the three "before" formats.
//   S_OK                         set_speaker_layout would call SetDeviceFormat
//   E_POINTER, E_INVALIDARG      `out` null, `layout` not in the enum
//   ERROR_NOT_FOUND, ERROR_NOT_READY, ERROR_INVALID_DATA (as HRESULTs)
//                                as supported_speaker_layouts
//   AUDCLNT_E_UNSUPPORTED_FORMAT the output does not support the layout
//   kPolicyConfigMismatch        GetDeviceFormat and the property differ
//   any other failure            activating IAudioClient, IsFormatSupported, creating
//                                PolicyConfigClient (E_NOINTERFACE on Windows 10 1507 and
//                                1511), GetDeviceFormat or GetMixFormat, as returned
HRESULT check_speaker_layout(const std::wstring& endpoint, SpeakerLayout layout, LayoutChange* out);

// check_speaker_layout, then IPolicyConfig::SetDeviceFormat(device ID, endpoint,
// mix), then GetDeviceFormat and GetMixFormat read back. The read-back is
// compared field by field (channels, rate, bits, valid bits, subformat, mask)
// with what was passed.
//   S_OK                      the device format and mix format are the layout's
//   check_speaker_layout's failures, with set_called false: nothing was changed
//   SetDeviceFormat's failure, as returned, with set_called true
//   a GetDeviceFormat or GetMixFormat failure after the set, as returned
//   kLayoutReadBackMismatch   `error` says which format and field differ
// Calling it with the layout the output already has sets the same formats again.
HRESULT set_speaker_layout(const std::wstring& endpoint, SpeakerLayout layout, LayoutChange* out);

}  // namespace isotone::devices
