// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "loopback_capture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <malloc.h>
#include <vector>

#include "isotone/param_block.h"

namespace isotone::compat {
namespace {

enum class SampleKind { Float32, Int16, Int24, Int32, Unsupported };

SampleKind kind_of(const WAVEFORMATEX* wfx) {
    // Sample layout follows the container size; a 24-bit-valid sample in a
    // 32-bit container is left-justified and reads correctly as Int32.
    bool is_float = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    const uint16_t container = wfx->wBitsPerSample;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        if (!is_float && !IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            return SampleKind::Unsupported;
        }
    } else if (!is_float && wfx->wFormatTag != WAVE_FORMAT_PCM) {
        return SampleKind::Unsupported;
    }
    if (is_float) return wfx->wBitsPerSample == 32 ? SampleKind::Float32 : SampleKind::Unsupported;
    switch (container) {
        case 16: return SampleKind::Int16;
        case 24: return SampleKind::Int24;
        case 32: return SampleKind::Int32;
        default: return SampleKind::Unsupported;
    }
}

void to_float(SampleKind kind, const BYTE* src, size_t samples, float* dst) {
    switch (kind) {
        case SampleKind::Float32:
            std::memcpy(dst, src, samples * sizeof(float));
            break;
        case SampleKind::Int16:
            for (size_t i = 0; i < samples; ++i) {
                int16_t v;
                std::memcpy(&v, src + i * 2, 2);
                dst[i] = v / 32768.0f;
            }
            break;
        case SampleKind::Int24:
            for (size_t i = 0; i < samples; ++i) {
                const BYTE* p = src + i * 3;
                const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 |
                                                       static_cast<uint32_t>(p[1]) << 16 |
                                                       static_cast<uint32_t>(p[2]) << 24) >> 8;
                dst[i] = static_cast<float>(v / 8388608.0);
            }
            break;
        case SampleKind::Int32:
            for (size_t i = 0; i < samples; ++i) {
                int32_t v;
                std::memcpy(&v, src + i * 4, 4);
                dst[i] = static_cast<float>(v / 2147483648.0);
            }
            break;
        case SampleKind::Unsupported:
            break;
    }
}

std::wstring device_id_for(const std::string& endpoint) {
    std::string id;
    if (endpoint.find("}.{") != std::string::npos) {
        id = endpoint;
    } else {
        std::string guid;
        for (char c : endpoint) {
            if (c != '{' && c != '}' && !std::isspace(static_cast<unsigned char>(c))) {
                guid += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        id = "{0.0.0.00000000}.{" + guid + "}";   // render endpoints carry the 0.0.0 flow prefix
    }
    return std::wstring(id.begin(), id.end());
}

}  // namespace

bool counts_as_glitch(DWORD flags, bool first_packet_after_start) {
    return (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && !first_packet_after_start;
}

struct LoopbackCapture::State {
    ~State() {
        if (region != nullptr) _aligned_free(region);
        if (ready != nullptr) CloseHandle(ready);
    }
    void* region = nullptr;   // AudioRingHeader then samples
    HANDLE ready = nullptr;   // set when the stream is running or has failed
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<HRESULT> thread_error{S_OK};
    std::atomic<uint32_t> discontinuities{0};
    std::atomic<uint32_t> sample_rate{0};
    std::atomic<uint32_t> channels{0};
};

HRESULT LoopbackCapture::start(const std::string& endpoint, DWORD timeout_ms) {
    stop();
    auto s = std::make_shared<State>();
    const size_t bytes = sizeof(AudioRingHeader) + size_t{kRingCapacityFrames} * kMaxChannels * sizeof(float);
    s->region = _aligned_malloc(bytes, alignof(AudioRingHeader));
    if (s->region == nullptr) return E_OUTOFMEMORY;
    std::memset(s->region, 0, bytes);
    audio_ring_init(static_cast<AudioRingHeader*>(s->region), kRingCapacityFrames);

    s->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (s->ready == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = s;
    }
    thread_ = std::thread(&LoopbackCapture::run, s, device_id_for(endpoint));
    const DWORD waited = WaitForSingleObject(s->ready, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        const HRESULT hr = HRESULT_FROM_WIN32(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        stop();
        return hr;
    }

    if (!s->running) {
        const HRESULT hr = s->thread_error.load();
        stop();
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

void LoopbackCapture::stop() {
    std::shared_ptr<State> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.swap(state_);
    }
    if (s) s->stop = true;
    if (thread_.joinable()) {
        // A thread that has not signalled may be blocked in the audio service
        // for as long as that takes; it holds the state, so it can be left.
        if (s && WaitForSingleObject(s->ready, 0) == WAIT_OBJECT_0) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }
}

std::shared_ptr<LoopbackCapture::State> LoopbackCapture::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool LoopbackCapture::running() const {
    const auto s = state();
    return s && s->running.load();
}

uint32_t LoopbackCapture::sample_rate() const {
    const auto s = state();
    return s ? s->sample_rate.load() : 0;
}

uint32_t LoopbackCapture::channels() const {
    const auto s = state();
    return s ? s->channels.load() : 0;
}

HRESULT LoopbackCapture::thread_error() const {
    const auto s = state();
    return s ? s->thread_error.load() : S_OK;
}

uint32_t LoopbackCapture::discontinuities() const {
    const auto s = state();
    return s ? s->discontinuities.load() : 0;
}

uint32_t LoopbackCapture::read(AudioRingCursor* cursor, float* out, uint32_t max_frames,
                               uint32_t* channels) const {
    const auto s = state();
    if (!s) return 0;
    return audio_ring_read(static_cast<const AudioRingHeader*>(s->region), kRingCapacityFrames, cursor, out, max_frames, channels);
}

void LoopbackCapture::run(std::shared_ptr<State> s, std::wstring device_id) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;

    const auto fail = [&](HRESULT hr) {
        s->thread_error = hr;
        return hr;
    };

    HRESULT hr = co;
    SampleKind kind = SampleKind::Unsupported;
    AudioRingWriter writer;
    uint32_t channels = 0;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    }
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(device_id.c_str(), &device);
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
    }
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) {
        kind = kind_of(format);
        if (kind == SampleKind::Unsupported) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (SUCCEEDED(hr)) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                2000000 /* 200 ms */, 0, format, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    }
    if (SUCCEEDED(hr)) {
        channels = format->nChannels;
        s->sample_rate = format->nSamplesPerSec;
        s->channels = channels;
        writer.attach(static_cast<AudioRingHeader*>(s->region), kRingCapacityFrames);
        writer.set_channels(channels);
        writer.claim((uint64_t{GetCurrentProcessId()} << 32) | 1u);
        hr = client->Start();
    }

    if (FAILED(hr)) {
        fail(hr);
        SetEvent(s->ready);
    } else {
        s->running = true;
        SetEvent(s->ready);

        std::vector<float> scratch;
        bool first_packet = true;
        while (!s->stop) {
            Sleep(5);
            for (;;) {
                UINT32 packet = 0;
                if (FAILED(hr = capture->GetNextPacketSize(&packet))) break;
                if (packet == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                if (counts_as_glitch(flags, first_packet)) s->discontinuities.fetch_add(1);
                first_packet = false;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    writer.write(nullptr, channels, frames);
                } else {
                    scratch.resize(size_t{frames} * channels);
                    to_float(kind, data, scratch.size(), scratch.data());
                    writer.write(scratch.data(), channels, frames);
                }
                capture->ReleaseBuffer(frames);
            }
            if (FAILED(hr)) {
                // The endpoint went away or changed format; the reader sees no
                // more frames and thread_error() says why.
                fail(hr);
                break;
            }
        }
        client->Stop();
        s->running = false;
    }

    if (capture) capture->Release();
    if (client) client->Release();
    if (format) CoTaskMemFree(format);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace isotone::compat
