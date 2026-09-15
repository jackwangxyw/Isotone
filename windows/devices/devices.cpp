// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "devices.h"

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <objbase.h>
#include <propidl.h>

#include <cstdio>
#include <cstring>

#include "shared_mapping.h"
#include "isotone/apo_config.h"

namespace isotone::devices {
namespace {

// Defined here rather than taken from the SDK headers, which only declare them
// unless INITGUID is set.
const PROPERTYKEY kDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kDeviceDesc = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
// Not PKEY_DeviceInterface_FriendlyName ({026e516e-...},2), which on this
// machine reads "2- AMD High Definition Audio Device" or nothing where
// devicetool shows "AMD High Definition Audio Device": this is the value
// upstream's DeviceAPOInfo reads from the endpoint's Properties key.
const PROPERTYKEY kAdapterName = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};
const PROPERTYKEY kEndpointGuid = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};

// The CLSIDs devicetool classifies (upstream/helpers/RegistryHelper.h).
const GUID kIsoApoPreMix = {0xf1dffd14, 0x9a30, 0x45c5, {0xba, 0xb2, 0xc8, 0x20, 0xc7, 0xec, 0x71, 0x8f}};
const GUID kIsoApoPostMix = {0xbaf30f18, 0x9fa2, 0x4e55, {0x97, 0xd9, 0x00, 0x7c, 0xea, 0x17, 0x98, 0x24}};
const GUID kEapoPreMix = {0xeacd2258, 0xfcac, 0x4ff4, {0xb3, 0x6d, 0x41, 0x9e, 0x92, 0x4a, 0x6d, 0x79}};
const GUID kEapoPostMix = {0xec1cc9ce, 0xfaed, 0x4822, {0x82, 0x8a, 0x82, 0xa8, 0x1a, 0x6f, 0x01, 0x8f}};

const wchar_t kRenderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
const wchar_t kIsoChildApos[] = L"SOFTWARE\\IsoAPO\\Child APOs";
const wchar_t kEapoChildApos[] = L"SOFTWARE\\EqualizerAPO\\Child APOs";
const wchar_t kPending[] = L"SOFTWARE\\IsoAPO\\Pending";
// devicetool's kEffectSlots and kEffectLists.
const wchar_t* const kSlots[] = {
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7",
};
const wchar_t* const kLists[] = {
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},19",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},20",
};
const wchar_t kDisableEnhancements[] = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

constexpr REGSAM kRead = KEY_QUERY_VALUE | KEY_WOW64_64KEY;

std::string hresult_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buf;
}

// devicetool's guid_equals, which uses CLSIDFromString too.
bool clsid_is(const std::wstring& text, const GUID& a, const GUID& b) {
    GUID parsed;
    return SUCCEEDED(CLSIDFromString(text.c_str(), &parsed)) && (parsed == a || parsed == b);
}

void classify_clsid(const std::wstring& clsid, EngineInfo* info) {
    if (clsid_is(clsid, kIsoApoPreMix, kIsoApoPostMix)) info->isoapo_in_slots = true;
    if (clsid_is(clsid, kEapoPreMix, kEapoPostMix)) info->equalizerapo_in_slots = true;
}

// A value of exactly `type`, or false. RegGetValueW terminates strings.
bool read_value(HKEY key, const wchar_t* name, DWORD flags, std::vector<wchar_t>* out) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    out->assign(bytes / sizeof(wchar_t) + 2, L'\0');
    bytes = static_cast<DWORD>(out->size() * sizeof(wchar_t));
    return RegGetValueW(key, nullptr, name, flags, nullptr, out->data(), &bytes) == ERROR_SUCCESS;
}

bool key_exists(HKEY root, const std::wstring& path) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, kRead, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

std::wstring property_string(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal != nullptr) s = v.pwszVal;
    PropVariantClear(&v);
    return s;
}

DeviceFormat format_from_store(IPropertyStore* store) {
    PROPVARIANT v;
    PropVariantInit(&v);
    DeviceFormat f;
    const HRESULT hr = store->GetValue(kDeviceFormat, &v);
    if (FAILED(hr)) {
        f.error = hresult_text("reading PKEY_AudioEngine_DeviceFormat", hr);
    } else if (v.vt == VT_EMPTY) {
        f.error = "no PKEY_AudioEngine_DeviceFormat";
    } else if (v.vt != VT_BLOB) {
        f.error = "PKEY_AudioEngine_DeviceFormat is not a blob";
    } else {
        f = parse_device_format(v.blob.pBlobData, v.blob.cbSize);
    }
    PropVariantClear(&v);
    return f;
}

std::wstring default_id(IMMDeviceEnumerator* enumerator, ERole role) {
    IMMDevice* device = nullptr;
    std::wstring id;
    // E_NOTFOUND when there is no render device at all: no default, not an error.
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) {
        LPWSTR text = nullptr;
        if (SUCCEEDED(device->GetId(&text))) id = text;
        CoTaskMemFree(text);
        device->Release();
    }
    return id;
}

void fill_endpoint(IMMDevice* device, const std::wstring& console, const std::wstring& multimedia, Endpoint* e) {
    *e = Endpoint{};
    LPWSTR text = nullptr;
    HRESULT hr = device->GetId(&text);
    if (FAILED(hr)) {
        e->error = hresult_text("IMMDevice::GetId", hr);
        return;
    }
    e->id = text;
    CoTaskMemFree(text);
    e->default_console = _wcsicmp(e->id.c_str(), console.c_str()) == 0;
    e->default_multimedia = _wcsicmp(e->id.c_str(), multimedia.c_str()) == 0;

    hr = device->GetState(&e->state);
    if (FAILED(hr)) e->error = hresult_text("IMMDevice::GetState", hr);

    IPropertyStore* store = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &store);
    if (FAILED(hr)) {
        e->error = hresult_text("IMMDevice::OpenPropertyStore", hr);
        e->format.error = e->error;
        return;
    }
    e->guid = isotone::win::canonical_endpoint_guid(property_string(store, kEndpointGuid));
    e->friendly_name = property_string(store, kFriendlyName);
    e->device_name = property_string(store, kAdapterName);
    e->connection_name = property_string(store, kDeviceDesc);
    e->format = format_from_store(store);
    store->Release();

    if (e->guid.empty()) {
        e->error = "no PKEY_AudioEndpoint_GUID";
        e->engine.error = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    } else {
        e->engine = read_engine(e->guid);
    }
}

HRESULT create_enumerator(IMMDeviceEnumerator** out) {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                            reinterpret_cast<void**>(out));
}

// Visits every render endpoint in every state until `visit` returns false.
template <typename F>
HRESULT for_each_render_device(IMMDeviceEnumerator* enumerator, F&& visit) {
    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection);
    if (FAILED(hr)) return hr;
    UINT count = 0;
    hr = collection->GetCount(&count);
    for (UINT i = 0; SUCCEEDED(hr) && i < count; ++i) {
        IMMDevice* device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) break;
        const bool more = visit(device);
        device->Release();
        if (!more) break;
    }
    collection->Release();
    return hr;
}

}  // namespace

DeviceFormat parse_device_format(const uint8_t* data, size_t size) {
    DeviceFormat f;
    WAVEFORMATEX wfx{};
    if (data == nullptr || size < sizeof(WAVEFORMATEX)) {
        f.error = "format blob of " + std::to_string(size) + " bytes is shorter than WAVEFORMATEX";
        return f;
    }
    std::memcpy(&wfx, data, sizeof(wfx));
    if (size < sizeof(WAVEFORMATEX) + wfx.cbSize) {
        f.error = "format blob of " + std::to_string(size) + " bytes is shorter than its cbSize of " +
                  std::to_string(wfx.cbSize) + " needs";
        return f;
    }
    if (wfx.nChannels == 0 || wfx.nSamplesPerSec == 0) {
        f.error = "format has no channels or no sample rate";
        return f;
    }

    f.channels = wfx.nChannels;
    f.sample_rate = wfx.nSamplesPerSec;
    f.bits_per_sample = wfx.wBitsPerSample;
    f.valid_bits = wfx.wBitsPerSample;
    if (wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wfx.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            f = DeviceFormat{};
            f.error = "extensible format with cbSize " + std::to_string(wfx.cbSize);
            return f;
        }
        WAVEFORMATEXTENSIBLE ext{};
        std::memcpy(&ext, data, sizeof(ext));
        f.valid_bits = ext.Samples.wValidBitsPerSample;
        f.channel_mask = ext.dwChannelMask;
        f.sample_format = IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)          ? SampleFormat::pcm
                          : IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? SampleFormat::ieee_float
                                                                                         : SampleFormat::other;
    } else {
        f.sample_format = wfx.wFormatTag == WAVE_FORMAT_PCM          ? SampleFormat::pcm
                          : wfx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? SampleFormat::ieee_float
                                                                     : SampleFormat::other;
    }
    if (f.channel_mask == 0) {
        f.channel_mask = isotone::default_speaker_mask(f.channels);
        f.mask_defaulted = true;
    }
    f.present = true;
    return f;
}

DeviceFormat read_device_format(const std::wstring& device_id) {
    DeviceFormat f;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = create_enumerator(&enumerator);
    if (FAILED(hr)) {
        f.error = hresult_text("creating IMMDeviceEnumerator", hr);
        return f;
    }
    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(device_id.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr)) {
        f.error = hresult_text("IMMDeviceEnumerator::GetDevice", hr);
        return f;
    }
    IPropertyStore* store = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &store);
    device->Release();
    if (FAILED(hr)) {
        f.error = hresult_text("IMMDevice::OpenPropertyStore", hr);
        return f;
    }
    f = format_from_store(store);
    store->Release();
    return f;
}

EngineInfo read_engine(const std::wstring& endpoint) {
    EngineInfo info;
    const std::wstring guid = isotone::win::canonical_endpoint_guid(endpoint);
    if (guid.empty()) {
        info.error = E_INVALIDARG;
        return info;
    }
    const std::wstring key = std::wstring(kRenderKey) + guid;
    if (!key_exists(HKEY_LOCAL_MACHINE, key)) {
        info.error = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return info;
    }

    HKEY fx = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, (key + L"\\FxProperties").c_str(), 0, kRead, &fx);
    if (status == ERROR_SUCCESS) {
        std::vector<wchar_t> buf;
        for (const wchar_t* slot : kSlots) {
            if (read_value(fx, slot, RRF_RT_REG_SZ | RRF_NOEXPAND, &buf)) classify_clsid(buf.data(), &info);
        }
        for (const wchar_t* list : kLists) {
            if (!read_value(fx, list, RRF_RT_REG_MULTI_SZ, &buf)) continue;
            for (const wchar_t* p = buf.data(); *p != L'\0'; p += std::wcslen(p) + 1) classify_clsid(p, &info);
        }
        DWORD disabled = 0;
        DWORD bytes = sizeof(disabled);
        if (RegGetValueW(fx, nullptr, kDisableEnhancements, RRF_RT_REG_DWORD, nullptr, &disabled, &bytes) ==
            ERROR_SUCCESS) {
            info.enhancements_disabled = disabled != 0;
        }
        RegCloseKey(fx);
    } else if (status != ERROR_FILE_NOT_FOUND) {
        info.error = HRESULT_FROM_WIN32(status);
        return info;
    }

    // devicetool's read_record: the record key, or upstream's version-0 form, a
    // REG_SZ value named by the GUID.
    info.isoapo_record = key_exists(HKEY_LOCAL_MACHINE, std::wstring(kIsoChildApos) + L"\\" + guid);
    if (!info.isoapo_record) {
        HKEY base = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIsoChildApos, 0, kRead, &base) == ERROR_SUCCESS) {
            std::vector<wchar_t> buf;
            info.isoapo_record = read_value(base, guid.c_str(), RRF_RT_REG_SZ | RRF_NOEXPAND, &buf);
            RegCloseKey(base);
        }
    }

    // devicetool's equalizerapo_over_isoapo: REG_SZ values of Equalizer APO's
    // record key (try_read_string), classified as read_slots classifies.
    HKEY eapo = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(kEapoChildApos) + L"\\" + guid).c_str(), 0, kRead, &eapo) ==
        ERROR_SUCCESS) {
        std::vector<wchar_t> buf;
        const auto names_isoapo = [&](const wchar_t* name) {
            return read_value(eapo, name, RRF_RT_REG_SZ | RRF_NOEXPAND, &buf) &&
                   clsid_is(buf.data(), kIsoApoPreMix, kIsoApoPostMix);
        };
        for (const wchar_t* slot : kSlots) info.equalizerapo_over_isoapo = info.equalizerapo_over_isoapo || names_isoapo(slot);
        for (const wchar_t* child : {L"PreMixChild", L"PostMixChild"})
            info.equalizerapo_over_isoapo = info.equalizerapo_over_isoapo || names_isoapo(child);
        RegCloseKey(eapo);
    }

    // devicetool's journal_key.
    info.journal = key_exists(HKEY_LOCAL_MACHINE, std::wstring(kPending) + L"\\" + guid);

    info.backend = classify_backend(info);
    info.isoapo_state = classify_isoapo_state(info);
    return info;
}

// windows/devicetool/main.cpp cmd_list and cmd_status (lines 622 and 692).
Backend classify_backend(const EngineInfo& f) {
    return f.isoapo_in_slots && f.equalizerapo_in_slots ? Backend::conflict
           : f.isoapo_in_slots                          ? Backend::native
           : f.equalizerapo_in_slots                    ? Backend::equalizerapo
                                                        : Backend::none;
}

// windows/devicetool/main.cpp isoapo_state (lines 1733 to 1754), without the
// remedies: its branches that differ only in remedies are one state here.
IsoApoState classify_isoapo_state(const EngineInfo& f) {
    if (f.journal) return IsoApoState::interrupted;                                            // 1736
    if (f.isoapo_in_slots && !f.isoapo_record) return IsoApoState::unrecorded;                 // 1738
    if (f.isoapo_in_slots && f.equalizerapo_in_slots) return IsoApoState::alongside_equalizerapo;   // 1739
    if (f.isoapo_in_slots) return IsoApoState::installed;                                      // 1740 to 1741
    if (!f.isoapo_record) return IsoApoState::not_installed;                                   // 1748
    if (f.equalizerapo_over_isoapo) return IsoApoState::replaced_by_equalizerapo;              // 1749
    return IsoApoState::detached;                                                              // 1751 to 1753
}

const char* backend_name(Backend backend) {
    switch (backend) {
        case Backend::none: return "none";
        case Backend::native: return "native";
        case Backend::equalizerapo: return "equalizerapo";
        case Backend::conflict: return "conflict";
    }
    return "?";
}

const char* isoapo_state_name(IsoApoState state) {
    switch (state) {
        case IsoApoState::interrupted: return "interrupted";
        case IsoApoState::unrecorded: return "unrecorded";
        case IsoApoState::alongside_equalizerapo: return "alongside_equalizerapo";
        case IsoApoState::installed: return "installed";
        case IsoApoState::not_installed: return "not_installed";
        case IsoApoState::replaced_by_equalizerapo: return "replaced_by_equalizerapo";
        case IsoApoState::detached: return "detached";
    }
    return "?";
}

HRESULT enumerate_render_endpoints(std::vector<Endpoint>* out) {
    out->clear();
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = create_enumerator(&enumerator);
    if (FAILED(hr)) return hr;
    const std::wstring console = default_id(enumerator, eConsole);
    const std::wstring multimedia = default_id(enumerator, eMultimedia);
    hr = for_each_render_device(enumerator, [&](IMMDevice* device) {
        out->emplace_back();
        fill_endpoint(device, console, multimedia, &out->back());
        return true;
    });
    enumerator->Release();
    return hr;
}

HRESULT read_render_endpoint(const std::wstring& endpoint, Endpoint* out) {
    const std::wstring guid = isotone::win::canonical_endpoint_guid(endpoint);
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = create_enumerator(&enumerator);
    if (FAILED(hr)) return hr;
    const std::wstring console = default_id(enumerator, eConsole);
    const std::wstring multimedia = default_id(enumerator, eMultimedia);
    bool found = false;
    hr = for_each_render_device(enumerator, [&](IMMDevice* device) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) found = _wcsicmp(id, endpoint.c_str()) == 0;
        CoTaskMemFree(id);
        IPropertyStore* store = nullptr;
        if (!found && !guid.empty() && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            found = isotone::win::canonical_endpoint_guid(property_string(store, kEndpointGuid)) == guid;
            store->Release();
        }
        if (found) fill_endpoint(device, console, multimedia, out);
        return !found;
    });
    enumerator->Release();
    if (FAILED(hr)) return hr;
    return found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

}  // namespace isotone::devices
