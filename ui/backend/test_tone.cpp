// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "test_tone.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "devices.h"
#include "pink_noise.h"

namespace isotone::ui {

namespace {

// Releases a COM pointer or frees a CoTaskMem allocation at the end of a scope.
template <typename T>
struct Released {
    T* p = nullptr;
    ~Released() {
        if (p) p->Release();
    }
};

}  // namespace

TestTone::~TestTone() { stop(); }

void TestTone::start(const std::wstring& endpoint, uint32_t channel, Failure on_failure) {
    stop();
    stop_ = false;
    frames_ = 0;
    thread_ = std::thread([this, endpoint, channel, on_failure = std::move(on_failure)] { run(endpoint, channel, on_failure); });
}

void TestTone::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
}

void TestTone::run(std::wstring endpoint, uint32_t channel, Failure on_failure) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto fail = [&](HRESULT hr, const char* what) {
        if (on_failure) on_failure(hr, what);
    };
    [&] {
        isotone::devices::Endpoint info;
        HRESULT hr = isotone::devices::read_render_endpoint(endpoint, &info);
        if (FAILED(hr)) return fail(hr, "read_render_endpoint");

        Released<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator.p));
        if (FAILED(hr)) return fail(hr, "MMDeviceEnumerator");
        Released<IMMDevice> device;
        hr = enumerator.p->GetDevice(info.id.c_str(), &device.p);
        if (FAILED(hr)) return fail(hr, "GetDevice");
        Released<IAudioClient> client;
        hr = device.p->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client.p));
        if (FAILED(hr)) return fail(hr, "Activate IAudioClient");

        WAVEFORMATEX* format = nullptr;
        hr = client.p->GetMixFormat(&format);
        if (FAILED(hr)) return fail(hr, "GetMixFormat");
        struct Freed {
            WAVEFORMATEX* f;
            ~Freed() { CoTaskMemFree(f); }
        } freed{format};

        bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            is_float = IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }
        const uint32_t channels = format->nChannels;
        const uint32_t bytes = channels ? format->nBlockAlign / channels : 0;
        const bool writable = (is_float && format->wBitsPerSample == 32 && bytes == 4) ||
                              (!is_float && ((format->wBitsPerSample == 16 && bytes == 2) ||
                                             (format->wBitsPerSample == 24 && bytes == 3) ||
                                             ((format->wBitsPerSample == 24 || format->wBitsPerSample == 32) && bytes == 4)));
        // Writing nothing would play whatever the buffer held.
        if (!writable) return fail(AUDCLNT_E_UNSUPPORTED_FORMAT, "mix format");
        if (channel >= channels) return fail(E_INVALIDARG, "channel");

        hr = client.p->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /* 200 ms */, 0, format, nullptr);
        if (FAILED(hr)) return fail(hr, "Initialize");
        UINT32 buffer_frames = 0;
        hr = client.p->GetBufferSize(&buffer_frames);
        if (FAILED(hr)) return fail(hr, "GetBufferSize");
        Released<IAudioRenderClient> render;
        hr = client.p->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render.p));
        if (FAILED(hr)) return fail(hr, "GetService IAudioRenderClient");

        PinkNoise noise(format->nSamplesPerSec);
        const UINT32 lead = std::min<UINT32>(buffer_frames, format->nSamplesPerSec / 20);   // about 50 ms queued
        const auto write = [&](BYTE* base, size_t index, float v) {
            if (is_float) {
                reinterpret_cast<float*>(base)[index] = v;
            } else if (bytes == 2) {
                reinterpret_cast<int16_t*>(base)[index] = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
            } else if (bytes == 3) {
                const int32_t s = static_cast<int32_t>(std::clamp(v, -1.0f, 1.0f) * 8388607.0f);
                std::memcpy(base + index * 3, &s, 3);   // little-endian: the low three bytes
            } else {
                reinterpret_cast<int32_t*>(base)[index] = static_cast<int32_t>(std::clamp(static_cast<double>(v), -1.0, 1.0) * 2147483647.0);
            }
        };

        hr = client.p->Start();
        if (FAILED(hr)) return fail(hr, "Start");
        while (!stop_) {
            UINT32 padding = 0;
            hr = client.p->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            if (padding < lead) {
                const UINT32 n = lead - padding;
                BYTE* data = nullptr;
                hr = render.p->GetBuffer(n, &data);
                if (FAILED(hr)) break;
                std::memset(data, 0, static_cast<size_t>(n) * format->nBlockAlign);
                for (UINT32 i = 0; i < n; ++i) write(data, static_cast<size_t>(i) * channels + channel, noise.next());
                hr = render.p->ReleaseBuffer(n, 0);
                if (FAILED(hr)) break;
                frames_ += n;
            }
            Sleep(10);
        }
        client.p->Stop();
        if (FAILED(hr)) fail(hr, "render");
    }();
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace isotone::ui
