// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "speaker_layout.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <objbase.h>

#include <cstdio>

namespace isotone::devices {
namespace {

// The table against the SDK's names for the same masks.
static_assert(kSpeakerLayouts[0].mask == KSAUDIO_SPEAKER_STEREO);
static_assert(kSpeakerLayouts[1].mask == KSAUDIO_SPEAKER_2POINT1);
static_assert(kSpeakerLayouts[2].mask == KSAUDIO_SPEAKER_5POINT1_SURROUND);
static_assert(kSpeakerLayouts[3].mask == KSAUDIO_SPEAKER_7POINT1_SURROUND);

// The vtable in speaker_layout.h, with its sources. Only GetMixFormat,
// GetDeviceFormat and SetDeviceFormat are called; the rest hold their slots.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR device_id, WAVEFORMATEX** format) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR device_id, INT default_format, WAVEFORMATEX** format) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR device_id) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR device_id, WAVEFORMATEX* endpoint, WAVEFORMATEX* mix) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

std::string hresult_text(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buf;
}

// One WAVEFORMATEXTENSIBLE, or false when a size does not fit its field.
bool fill_format(uint16_t channels, uint32_t mask, uint32_t rate, uint32_t bits, uint32_t valid, const GUID& subformat,
                 WAVEFORMATEXTENSIBLE* f) {
    const uint32_t block = uint32_t{channels} * bits / 8;
    const uint64_t bytes_per_second = uint64_t{rate} * block;
    if (bits > 0xFFFF || valid > 0xFFFF || block > 0xFFFF || bytes_per_second > 0xFFFFFFFFu) return false;
    *f = WAVEFORMATEXTENSIBLE{};
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = channels;
    f->Format.nSamplesPerSec = rate;
    f->Format.wBitsPerSample = static_cast<WORD>(bits);
    f->Format.nBlockAlign = static_cast<WORD>(block);
    f->Format.nAvgBytesPerSec = static_cast<DWORD>(bytes_per_second);
    f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f->Samples.wValidBitsPerSample = static_cast<WORD>(valid);
    f->dwChannelMask = mask;
    f->SubFormat = subformat;
    return true;
}

HRESULT build_formats(const DeviceFormat& current, uint16_t channels, uint32_t mask, LayoutFormats* out) {
    const HRESULT invalid = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (!current.present || current.sample_format == SampleFormat::other) return invalid;
    if (current.bits_per_sample == 0 || current.bits_per_sample % 8 != 0) return invalid;
    if (current.valid_bits == 0 || current.valid_bits > current.bits_per_sample) return invalid;
    const GUID& subformat =
        current.sample_format == SampleFormat::pcm ? KSDATAFORMAT_SUBTYPE_PCM : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    LayoutFormats f;
    if (!fill_format(channels, mask, current.sample_rate, current.bits_per_sample, current.valid_bits, subformat,
                     &f.endpoint) ||
        !fill_format(channels, mask, current.sample_rate, 32, 32, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, &f.mix)) {
        return invalid;
    }
    *out = f;
    return S_OK;
}

DeviceFormat parse_format(const WAVEFORMATEX* f) {
    if (f == nullptr) return parse_device_format(nullptr, 0);
    return parse_device_format(reinterpret_cast<const uint8_t*>(f), sizeof(WAVEFORMATEX) + f->cbSize);
}

// The first field in which `got` differs from `want`, or empty.
std::string format_difference(const DeviceFormat& got, const DeviceFormat& want) {
    const auto field = [](const char* name, unsigned long g, unsigned long w) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s 0x%lx, expected 0x%lx", name, g, w);
        return std::string(buf);
    };
    if (got.present != want.present) return got.present ? "a format where none was expected" : "no format: " + got.error;
    if (got.channels != want.channels) return field("channels", got.channels, want.channels);
    if (got.sample_rate != want.sample_rate) return field("sample rate", got.sample_rate, want.sample_rate);
    if (got.bits_per_sample != want.bits_per_sample) return field("bits", got.bits_per_sample, want.bits_per_sample);
    if (got.valid_bits != want.valid_bits) return field("valid bits", got.valid_bits, want.valid_bits);
    if (got.sample_format != want.sample_format)
        return field("sample format", static_cast<unsigned long>(got.sample_format),
                     static_cast<unsigned long>(want.sample_format));
    if (got.channel_mask != want.channel_mask) return field("mask", got.channel_mask, want.channel_mask);
    if (got.mask_defaulted != want.mask_defaulted) return field("mask defaulted", got.mask_defaulted, want.mask_defaulted);
    return {};
}

// An active render endpoint.
HRESULT resolve(const std::wstring& endpoint, Endpoint* e, std::string* error) {
    HRESULT hr = read_render_endpoint(endpoint, e);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        *error = "no such render endpoint";
    } else if (FAILED(hr)) {
        *error = hresult_text("finding the render endpoint", hr);
    } else if (e->state != DEVICE_STATE_ACTIVE) {
        *error = "the endpoint is not active";
        hr = HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    return hr;
}

HRESULT activate_client(const std::wstring& device_id, IAudioClient** out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return hr;
    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(device_id.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr)) return hr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(out));
    device->Release();
    return hr;
}

// IsFormatSupported in exclusive mode: S_OK, AUDCLNT_E_UNSUPPORTED_FORMAT, or a
// failure. Exclusive mode answers only those two when it succeeds; any other
// success code is not documented for it and reads as E_UNEXPECTED.
HRESULT exclusive_supported(IAudioClient* client, const WAVEFORMATEXTENSIBLE& format) {
    const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
    if (hr == S_OK || hr == AUDCLNT_E_UNSUPPORTED_FORMAT || FAILED(hr)) return hr;
    return E_UNEXPECTED;
}

enum class PolicyRead { device, mix };

HRESULT policy_read(IPolicyConfig* policy, const std::wstring& device_id, PolicyRead which, DeviceFormat* out) {
    WAVEFORMATEX* f = nullptr;
    const HRESULT hr = which == PolicyRead::device ? policy->GetDeviceFormat(device_id.c_str(), 0, &f)
                                                   : policy->GetMixFormat(device_id.c_str(), &f);
    if (SUCCEEDED(hr)) *out = parse_format(f);
    CoTaskMemFree(f);
    return hr;
}

// check_speaker_layout, leaving the created IPolicyConfig in `policy` on S_OK.
HRESULT prepare(const std::wstring& endpoint, SpeakerLayout layout, LayoutChange* out, IPolicyConfig** policy) {
    *policy = nullptr;
    *out = LayoutChange{};
    if (speaker_layout_spec(layout) == nullptr) {
        out->error = "not a speaker layout";
        return E_INVALIDARG;
    }
    Endpoint e;
    HRESULT hr = resolve(endpoint, &e, &out->error);
    if (FAILED(hr)) return hr;
    out->device_id = e.id;
    out->before = e.format;
    hr = build_layout_formats(e.format, layout, &out->requested);
    if (FAILED(hr)) {
        out->error = "the current format cannot be the base: " + (e.format.present ? "unusable fields" : e.format.error);
        return hr;
    }

    IAudioClient* client = nullptr;
    hr = activate_client(e.id, &client);
    if (FAILED(hr)) {
        out->error = hresult_text("activating IAudioClient", hr);
        return hr;
    }
    hr = exclusive_supported(client, out->requested.endpoint);
    client->Release();
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        out->error = "the output does not support the layout";
        return hr;
    }
    if (FAILED(hr)) {
        out->error = hresult_text("IAudioClient::IsFormatSupported", hr);
        return hr;
    }

    IPolicyConfig* pc = nullptr;
    hr = CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                          reinterpret_cast<void**>(&pc));
    if (FAILED(hr)) {
        out->error = hresult_text("creating PolicyConfigClient", hr);
        return hr;
    }
    hr = policy_read(pc, e.id, PolicyRead::device, &out->policy_before);
    if (FAILED(hr)) {
        out->error = hresult_text("IPolicyConfig::GetDeviceFormat", hr);
    } else if (hr = policy_read(pc, e.id, PolicyRead::mix, &out->mix_before); FAILED(hr)) {
        out->error = hresult_text("IPolicyConfig::GetMixFormat", hr);
    } else if (const std::string diff = format_difference(out->policy_before, out->before); !diff.empty()) {
        out->error = "IPolicyConfig::GetDeviceFormat differs from PKEY_AudioEngine_DeviceFormat: " + diff;
        hr = kPolicyConfigMismatch;
    }
    if (FAILED(hr)) {
        pc->Release();
        return hr;
    }
    *policy = pc;
    return S_OK;
}

}  // namespace

bool current_speaker_layout(const DeviceFormat& format, SpeakerLayout* out) {
    if (!format.present) return false;
    for (const SpeakerLayoutSpec& spec : kSpeakerLayouts) {
        if (format.channels == spec.channels && format.channel_mask == spec.mask) {
            *out = spec.layout;
            return true;
        }
    }
    return false;
}

HRESULT build_layout_formats(const DeviceFormat& current, SpeakerLayout layout, LayoutFormats* out) {
    if (out == nullptr) return E_POINTER;
    const SpeakerLayoutSpec* spec = speaker_layout_spec(layout);
    if (spec == nullptr) return E_INVALIDARG;
    return build_formats(current, spec->channels, spec->mask, out);
}

HRESULT supported_speaker_layouts(const std::wstring& endpoint, std::vector<SpeakerLayout>* out) {
    if (out == nullptr) return E_POINTER;
    out->clear();
    Endpoint e;
    std::string ignored;
    HRESULT hr = resolve(endpoint, &e, &ignored);
    if (FAILED(hr)) return hr;
    LayoutFormats current;
    hr = build_formats(e.format, e.format.channels, e.format.channel_mask, &current);
    if (FAILED(hr)) return hr;

    IAudioClient* client = nullptr;
    hr = activate_client(e.id, &client);
    if (FAILED(hr)) return hr;
    hr = exclusive_supported(client, current.endpoint);
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) hr = kCurrentFormatRefused;
    std::vector<SpeakerLayout> found;
    for (const SpeakerLayoutSpec& spec : kSpeakerLayouts) {
        if (FAILED(hr)) break;
        LayoutFormats f;
        hr = build_formats(e.format, spec.channels, spec.mask, &f);
        if (FAILED(hr)) break;
        hr = exclusive_supported(client, f.endpoint);
        if (hr == S_OK) {
            found.push_back(spec.layout);
        } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            hr = S_OK;
        }
    }
    client->Release();
    if (FAILED(hr)) return hr;
    *out = found;
    return S_OK;
}

HRESULT check_speaker_layout(const std::wstring& endpoint, SpeakerLayout layout, LayoutChange* out) {
    if (out == nullptr) return E_POINTER;
    IPolicyConfig* policy = nullptr;
    const HRESULT hr = prepare(endpoint, layout, out, &policy);
    if (policy != nullptr) policy->Release();
    return hr;
}

HRESULT set_speaker_layout(const std::wstring& endpoint, SpeakerLayout layout, LayoutChange* out) {
    if (out == nullptr) return E_POINTER;
    IPolicyConfig* policy = nullptr;
    HRESULT hr = prepare(endpoint, layout, out, &policy);
    if (FAILED(hr)) return hr;

    out->set_called = true;
    hr = policy->SetDeviceFormat(out->device_id.c_str(), &out->requested.endpoint.Format, &out->requested.mix.Format);
    if (FAILED(hr)) {
        out->error = hresult_text("IPolicyConfig::SetDeviceFormat", hr);
    } else if (hr = policy_read(policy, out->device_id, PolicyRead::device, &out->after); FAILED(hr)) {
        out->error = hresult_text("IPolicyConfig::GetDeviceFormat after the set", hr);
    } else if (hr = policy_read(policy, out->device_id, PolicyRead::mix, &out->mix_after); FAILED(hr)) {
        out->error = hresult_text("IPolicyConfig::GetMixFormat after the set", hr);
    }
    policy->Release();
    out->property_after = read_device_format(out->device_id);
    if (FAILED(hr)) return hr;

    if (std::string diff = format_difference(out->after, parse_format(&out->requested.endpoint.Format)); !diff.empty()) {
        out->error = "device format read back: " + diff;
        return kLayoutReadBackMismatch;
    }
    if (std::string diff = format_difference(out->mix_after, parse_format(&out->requested.mix.Format)); !diff.empty()) {
        out->error = "mix format read back: " + diff;
        return kLayoutReadBackMismatch;
    }
    return S_OK;
}

}  // namespace isotone::devices
