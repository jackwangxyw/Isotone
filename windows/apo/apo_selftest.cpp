// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Hosts IsoAPO in this process and drives the real COM lifecycle: class
// factory, Initialize, LockForProcess, APOProcess, UnlockForProcess. Audio is
// pushed through it and measured, and the shared region is opened from the other
// side exactly as the UI will open it: parameters written under the seqlock,
// the audio ring drained, the heartbeat watched.
//
// The point is to find out whether the APO works before letting audiodg.exe load
// it. A fault here is a failed exit code; the same fault inside audiodg takes
// down every sound on the machine until the audio service restarts.
//
// Registration is not needed: the DLL is loaded directly and DllGetClassObject
// is called by hand, so this touches no registry key and no audio device. It
// loads IsoAPO-selftest.dll, the build of the same sources that puts the region
// in Local\ rather than Global\, because this process cannot create Global\
// objects without elevation.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <aclapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <sddl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "shared_mapping.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr UINT32 kChannels = 2;
constexpr UINT32 kMaxFrames = 1024;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

void check_hr(HRESULT hr, const char* what) {
    const bool ok = SUCCEEDED(hr);
    if (ok) {
        std::printf("  %-66s ok\n", what);
    } else {
        std::printf("  %-66s FAIL (0x%08lx)\n", what, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// Analytic response of a peaking filter, so the test compares against the maths
// rather than against a guessed tolerance. An RBJ bell is wide: a Q of 1 centred
// at 1 kHz still pulls 0.16 dB at 100 Hz, which is not "untouched" and should
// not be asserted as such.
double peaking_db(double fc, double gain_db, double q, double freq, double rate) {
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * fc / rate;
    const double sn = std::sin(w0), cs = std::cos(w0);
    const double alpha = sn / (2.0 * q);
    double b[3] = {1.0 + alpha * A, -2.0 * cs, 1.0 - alpha * A};
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cs / a0;
    const double a2 = (1.0 - alpha / A) / a0;
    for (double& v : b) v /= a0;

    const double w = 2.0 * kPi * freq / rate;
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);
    const double nr = b[0] + b[1] * cw + b[2] * c2w;
    const double ni = -(b[1] * sw + b[2] * s2w);
    const double dr = 1.0 + a1 * cw + a2 * c2w;
    const double di = -(a1 * sw + a2 * s2w);
    return 20.0 * std::log10(std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di)));
}

// Same single-frequency windowed DFT the measurement tool uses.
double amplitude_at(const float* x, size_t n, size_t stride, double freq, double rate) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double phase = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        re += x[i * stride] * w * std::cos(phase);
        im -= x[i * stride] * w * std::sin(phase);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// Pushes `total_frames` of a unit sine through the APO in blocks, in place, as
// the engine does, calling `after` with the frame count once each block is
// processed. Returns everything that came out, or an empty vector if the APO
// returned the wrong frame count.
std::vector<float> run_sine(IAudioProcessingObjectRT* rt, float* buffer, double freq,
                            size_t total_frames,
                            const std::function<void(UINT32)>& after = nullptr) {
    std::vector<float> captured;
    captured.reserve(total_frames * kChannels);
    size_t position = 0;
    while (position < total_frames) {
        const UINT32 frames = static_cast<UINT32>(
            (total_frames - position) < kMaxFrames ? (total_frames - position) : kMaxFrames);
        for (UINT32 i = 0; i < frames; ++i) {
            const double t = static_cast<double>(position + i);
            const float v = static_cast<float>(std::sin(2.0 * kPi * freq * t / kRate));
            for (UINT32 c = 0; c < kChannels; ++c) buffer[i * kChannels + c] = v;
        }
        APO_CONNECTION_PROPERTY in{};
        in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
        in.u32ValidFrameCount = frames;
        in.u32BufferFlags = BUFFER_VALID;
        in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY out = in;
        APO_CONNECTION_PROPERTY* ins[1] = {&in};
        APO_CONNECTION_PROPERTY* outs[1] = {&out};
        rt->APOProcess(1, ins, 1, outs);
        if (out.u32ValidFrameCount != frames) {
            return {};
        }
        captured.insert(captured.end(), buffer, buffer + static_cast<size_t>(frames) * kChannels);
        if (after) after(frames);
        position += frames;
    }
    return captured;
}

// Level at `freq` over the second half of a capture, once the filter has
// settled and any parameter ramp has finished.
double settled_db(const std::vector<float>& captured, double freq) {
    const size_t frames = captured.size() / kChannels;
    const size_t skip = frames / 2;
    return 20.0 * std::log10(
        amplitude_at(captured.data() + skip * kChannels, frames - skip, kChannels, freq, kRate));
}

// The endpoint property store audiodg hands an APO in APOInitSystemEffects.
// Only PKEY_AudioEndpoint_GUID is needed.
class FakeEndpointProperties : public IPropertyStore {
public:
    explicit FakeEndpointProperties(std::wstring guid) : guid_(std::move(guid)) {}

    HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IPropertyStore)) {
            *ppv = static_cast<IPropertyStore*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 2; }    // lives on the stack
    ULONG __stdcall Release() override { return 1; }

    HRESULT __stdcall GetCount(DWORD* count) override {
        *count = 1;
        return S_OK;
    }
    HRESULT __stdcall GetAt(DWORD, PROPERTYKEY*) override { return E_NOTIMPL; }
    HRESULT __stdcall SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    HRESULT __stdcall Commit() override { return S_OK; }

    HRESULT __stdcall GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        // PKEY_AudioEndpoint_GUID, from mmdeviceapi.h.
        static constexpr PROPERTYKEY kEndpointGuid = {
            {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};
        PropVariantInit(pv);
        if (!IsEqualPropertyKey(key, kEndpointGuid)) {
            return S_OK;   // absent properties come back VT_EMPTY
        }
        const size_t bytes = (guid_.size() + 1) * sizeof(wchar_t);
        pv->pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (pv->pwszVal == nullptr) return E_OUTOFMEMORY;
        std::memcpy(pv->pwszVal, guid_.c_str(), bytes);
        pv->vt = VT_LPWSTR;
        return S_OK;
    }

private:
    std::wstring guid_;
};

struct Apo {
    IAudioProcessingObject* apo = nullptr;
    IAudioProcessingObjectConfiguration* config = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;

    void release() {
        if (rt) rt->Release();
        if (config) config->Release();
        if (apo) apo->Release();
        rt = nullptr;
        config = nullptr;
        apo = nullptr;
    }
};

bool create_apo(IClassFactory* factory, Apo* out) {
    if (FAILED(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                       reinterpret_cast<void**>(&out->apo)))) {
        return false;
    }
    return SUCCEEDED(out->apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                              reinterpret_cast<void**>(&out->config))) &&
           SUCCEEDED(out->apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                              reinterpret_cast<void**>(&out->rt)));
}

void ui_write(isotone::ParamBlock* shared, const isotone::EqState& state) {
    isotone::param_block_write(shared, [&](isotone::ParamBlock* b) { isotone::to_param_block(state, b); });
}

isotone::EqState peaking_state(double fc, double gain_db, double q) {
    isotone::EqState s;
    isotone::Band band;
    band.id = 1;
    band.type = isotone::FilterType::Peaking;
    band.fc = fc;
    band.gain_db = gain_db;
    band.width = q;
    s.bands.push_back(band);
    return s;
}

// --serve <endpoint-guid> <seconds>: hosts one instance in real time, playing a
// 0.5 amplitude 1 kHz sine in 10 ms blocks, so another process can drive the
// region exactly as the UI will. Prints one JSON line a second with the level
// at 1 kHz over that second, relative to the input.
int serve(const std::wstring& guid, double seconds) {
    HMODULE module = LoadLibraryW(L"IsoAPO-selftest.dll");
    if (module == nullptr) return 2;
    auto get_class_object = reinterpret_cast<HRESULT(__stdcall*)(const CLSID&, const IID&, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    const CLSID post_mix = {0xbaf30f18, 0x9fa2, 0x4e55,
                            {0x97, 0xd9, 0x00, 0x7c, 0xea, 0x17, 0x98, 0x24}};
    IClassFactory* factory = nullptr;
    Apo apo;
    if (get_class_object == nullptr ||
        FAILED(get_class_object(post_mix, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory))) ||
        !create_apo(factory, &apo)) {
        return 2;
    }

    FakeEndpointProperties properties(guid);
    APOInitSystemEffects init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = post_mix;
    init.pAPOEndpointProperties = &properties;
    if (FAILED(apo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)))) return 2;

    constexpr UINT32 kBlock = 480;
    UNCOMPRESSEDAUDIOFORMAT format{};
    format.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    format.dwSamplesPerFrame = kChannels;
    format.dwBytesPerSampleContainer = 4;
    format.dwValidBitsPerSample = 32;
    format.fFramesPerSecond = static_cast<float>(kRate);
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    IAudioMediaType* media = nullptr;
    std::vector<float> buffer(size_t{kBlock} * kChannels);
    if (FAILED(CreateAudioMediaTypeFromUncompressedAudioFormat(&format, &media))) return 2;
    APO_CONNECTION_DESCRIPTOR descriptor{};
    descriptor.Type = APO_CONNECTION_BUFFER_TYPE_ALLOCATED;
    descriptor.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    descriptor.u32MaxFrameCount = kBlock;
    descriptor.pFormat = media;
    descriptor.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    APO_CONNECTION_DESCRIPTOR* descriptors[1] = {&descriptor};
    if (FAILED(apo.config->LockForProcess(1, descriptors, 1, descriptors))) return 2;

    const size_t per_second = static_cast<size_t>(kRate);
    std::vector<float> second;
    second.reserve(per_second);
    const auto start = std::chrono::steady_clock::now();
    const uint64_t total_blocks = static_cast<uint64_t>(seconds * kRate / kBlock);
    uint64_t position = 0;
    for (uint64_t k = 0; k < total_blocks; ++k) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(10 * k));
        for (UINT32 i = 0; i < kBlock; ++i) {
            const float v = static_cast<float>(
                0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(position + i) / kRate));
            for (UINT32 c = 0; c < kChannels; ++c) buffer[size_t{i} * kChannels + c] = v;
        }
        APO_CONNECTION_PROPERTY in{};
        in.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
        in.u32ValidFrameCount = kBlock;
        in.u32BufferFlags = BUFFER_VALID;
        in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY out = in;
        APO_CONNECTION_PROPERTY* ins[1] = {&in};
        APO_CONNECTION_PROPERTY* outs[1] = {&out};
        apo.rt->APOProcess(1, ins, 1, outs);
        position += kBlock;

        for (UINT32 i = 0; i < kBlock; ++i) second.push_back(buffer[size_t{i} * kChannels]);
        if (second.size() >= per_second) {
            const double level =
                20.0 * std::log10(amplitude_at(second.data(), second.size(), 1, 1000.0, kRate) / 0.5);
            std::printf("{\"t\":%.2f,\"level_db_1k\":%.3f}\n",
                        static_cast<double>(position) / kRate, level);
            std::fflush(stdout);
            second.clear();
        }
    }

    apo.config->UnlockForProcess();
    apo.release();
    media->Release();
    factory->Release();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--serve") == 0) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
        const std::string guid = argv[2];
        return serve(std::wstring(guid.begin(), guid.end()), std::atof(argv[3]));
    }

    const std::wstring dll = argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                                      : L"IsoAPO-selftest.dll";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::fprintf(stderr, "CoInitializeEx failed\n");
        return 2;
    }

    std::printf("IsoAPO self test\n");

    HMODULE module = LoadLibraryW(dll.c_str());
    check(module != nullptr, "LoadLibrary IsoAPO-selftest.dll");
    if (module == nullptr) {
        std::fprintf(stderr, "  GetLastError = %lu\n", GetLastError());
        return 2;
    }

    using GetClassObjectFn = HRESULT(__stdcall*)(const CLSID&, const IID&, void**);
    using CanUnloadFn = HRESULT(__stdcall*)();
    auto get_class_object =
        reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    auto can_unload = reinterpret_cast<CanUnloadFn>(GetProcAddress(module, "DllCanUnloadNow"));
    check(get_class_object != nullptr, "DllGetClassObject is exported");
    check(can_unload != nullptr, "DllCanUnloadNow is exported");
    check(GetProcAddress(module, "DllRegisterServer") != nullptr,
          "DllRegisterServer is exported");
    check(GetProcAddress(module, "DllUnregisterServer") != nullptr,
          "DllUnregisterServer is exported");
    if (get_class_object == nullptr || can_unload == nullptr) {
        return 2;
    }

    // {BAF30F18-9FA2-4E55-97D9-007CEA179824}
    const CLSID post_mix = {0xbaf30f18, 0x9fa2, 0x4e55,
                            {0x97, 0xd9, 0x00, 0x7c, 0xea, 0x17, 0x98, 0x24}};
    const CLSID bogus = {0xdeadbeef, 0x0000, 0x0000,
                         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    IClassFactory* factory = nullptr;
    hr = get_class_object(post_mix, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    check_hr(hr, "DllGetClassObject for the post-mix CLSID");
    if (FAILED(hr)) return 2;

    void* ignored = nullptr;
    check(get_class_object(bogus, __uuidof(IClassFactory), &ignored) ==
              CLASS_E_CLASSNOTAVAILABLE,
          "an unknown CLSID is refused");

    check(can_unload() == S_OK, "DllCanUnloadNow says yes while nothing is live");

    Apo first;
    check(create_apo(factory, &first), "CreateInstance and the configuration and RT interfaces");
    if (first.rt == nullptr) return 2;
    check(can_unload() == S_FALSE, "DllCanUnloadNow says no while an instance is live");

    IAudioSystemEffects* effects = nullptr;
    check_hr(first.apo->QueryInterface(__uuidof(IAudioSystemEffects),
                                       reinterpret_cast<void**>(&effects)),
             "QueryInterface IAudioSystemEffects");
    if (effects) effects->Release();

    HNSTIME latency = -1;
    check_hr(first.apo->GetLatency(&latency), "GetLatency");
    check(latency == 0, "latency is zero, as a biquad cascade should be");

    // ------------------------------------------------------------------
    std::printf("\nshared region\n");

    // A fresh endpoint GUID per run, so parallel runs cannot collide. Upper
    // case, as PKEY_AudioEndpoint_GUID delivers it.
    GUID endpoint{};
    CoCreateGuid(&endpoint);
    wchar_t endpoint_upper[64] = {};
    StringFromGUID2(endpoint, endpoint_upper, 64);
    const std::wstring name = isotone::win::mapping_name(L"Local\\", endpoint_upper);

    isotone::win::SharedMapping ui;
    check(ui.open(name) == ERROR_FILE_NOT_FOUND, "no region exists before Initialize");

    check(first.apo->Initialize(0, nullptr) == E_INVALIDARG,
          "Initialize without system-effects data is refused");

    FakeEndpointProperties endpoint_properties(endpoint_upper);
    APOInitSystemEffects init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = post_mix;
    init.pAPOEndpointProperties = &endpoint_properties;
    check_hr(first.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
             "Initialize with the endpoint's property store");

    check(ui.open(name) == ERROR_SUCCESS, "the UI side opens the region by its lower-case name");
    if (!ui.is_open()) return 2;
    isotone::ParamBlock* shared = ui.params();
    isotone::AudioRingHeader* ring = ui.ring();

    check(shared->hdr.host_state == static_cast<uint32_t>(isotone::HostState::NotLoaded),
          "host state is NotLoaded before LockForProcess");
    check(shared->band_count == 1 && shared->bands[0].gain_db == -12.0f,
          "the new region is seeded with the default -12 dB band");

    {
        HANDLE h = OpenFileMappingW(READ_CONTROL, FALSE, name.c_str());
        PSECURITY_DESCRIPTOR sd = nullptr;
        LPWSTR sddl = nullptr;
        const bool read =
            h != nullptr &&
            GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            nullptr, nullptr, &sd) == ERROR_SUCCESS &&
            ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
                                                                 DACL_SECURITY_INFORMATION, &sddl,
                                                                 nullptr);
        const std::wstring acl = read ? sddl : L"";
        std::printf("  mapping DACL: %ls\n", acl.c_str());
        check(acl.rfind(L"D:P", 0) == 0 && acl.find(L";;;SY)") != std::wstring::npos &&
                  acl.find(L";;;LS)") != std::wstring::npos &&
                  acl.find(L";;;AU)") != std::wstring::npos &&
                  acl.find(L";;;WD)") == std::wstring::npos,
              "DACL is protected: SYSTEM, LocalService, Authenticated Users only");
        if (sddl) LocalFree(sddl);
        if (sd) LocalFree(sd);
        if (h) CloseHandle(h);
    }

    // Build the format the audio engine would negotiate: 48 kHz, stereo, float32.
    UNCOMPRESSEDAUDIOFORMAT format{};
    format.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    format.dwSamplesPerFrame = kChannels;
    format.dwBytesPerSampleContainer = 4;
    format.dwValidBitsPerSample = 32;
    format.fFramesPerSecond = static_cast<float>(kRate);
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    IAudioMediaType* media = nullptr;
    check_hr(CreateAudioMediaTypeFromUncompressedAudioFormat(&format, &media),
             "CreateAudioMediaTypeFromUncompressedAudioFormat");
    if (media == nullptr) return 2;

    const size_t sample_count = static_cast<size_t>(kMaxFrames) * kChannels;
    float* buffer = static_cast<float*>(_aligned_malloc(sample_count * sizeof(float), 32));
    if (buffer == nullptr) return 2;

    APO_CONNECTION_DESCRIPTOR descriptor{};
    descriptor.Type = APO_CONNECTION_BUFFER_TYPE_ALLOCATED;
    descriptor.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
    descriptor.u32MaxFrameCount = kMaxFrames;
    descriptor.pFormat = media;
    descriptor.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    APO_CONNECTION_DESCRIPTOR* descriptors[1] = {&descriptor};

    check_hr(first.config->LockForProcess(1, descriptors, 1, descriptors), "LockForProcess");
    check(shared->hdr.sample_rate == 48000 && shared->hdr.channels == 2,
          "LockForProcess publishes the rate and channel count");
    check(shared->hdr.host_state == static_cast<uint32_t>(isotone::HostState::Running),
          "host state is Running after LockForProcess");
    check(ring->writer != 0 && ring->channels == 2 && (ring->epoch & 1u) == 0,
          "the instance claims the audio ring for stereo");

    // ------------------------------------------------------------------
    std::printf("\naudio path\n");

    isotone::AudioRingCursor cursor;
    std::vector<float> drained(static_cast<size_t>(kMaxFrames) * isotone::kMaxChannels);
    uint32_t ring_channels = 0;
    isotone::audio_ring_read(ring, &cursor, drained.data(), kMaxFrames, &ring_channels);

    // Drain the ring after every block, as the UI's timer would, and demand the
    // ring hold exactly what the APO output.
    bool ring_matches = true;
    size_t ring_frames = 0;
    const auto drain_and_compare = [&](UINT32 frames) {
        const uint32_t n =
            isotone::audio_ring_read(ring, &cursor, drained.data(), kMaxFrames, &ring_channels);
        ring_frames += n;
        ring_matches &= n == frames && ring_channels == kChannels &&
                        std::memcmp(drained.data(), buffer,
                                    static_cast<size_t>(frames) * kChannels * sizeof(float)) == 0;
    };

    const uint32_t beats_before = shared->hdr.host_heartbeat;
    std::vector<float> captured = run_sine(first.rt, buffer, 1000.0, 48000, drain_and_compare);
    check(captured.size() == 48000u * kChannels, "one second of audio came back");
    const uint32_t calls = (48000 + kMaxFrames - 1) / kMaxFrames;
    check(shared->hdr.host_heartbeat - beats_before == calls,
          "the heartbeat advanced once per process call");
    check(ring_matches && ring_frames == 48000,
          "the ring carried every output frame, bit for bit");

    const double db_1k = settled_db(captured, 1000.0);
    std::printf("  %-66s %+.3f dB (analytic %+.3f)\n", "measured level at 1 kHz", db_1k,
                peaking_db(1000.0, -12.0, 1.0, 1000.0, kRate));
    check(std::abs(db_1k + 12.0) < 0.01, "the seeded -12 dB dip at 1 kHz is present");

    captured = run_sine(first.rt, buffer, 100.0, 48000);
    const double db_100 = settled_db(captured, 100.0);
    const double expect_100 = peaking_db(1000.0, -12.0, 1.0, 100.0, kRate);
    std::printf("  %-66s %+.3f dB (analytic %+.3f)\n", "measured level at 100 Hz", db_100,
                expect_100);
    check(std::abs(db_100 - expect_100) < 0.01, "100 Hz matches the analytic skirt of the band");

    // A silent buffer must come back flagged silent, or some drivers misbehave,
    // and the ring must still advance so the spectrum shows silence.
    {
        for (size_t i = 0; i < sample_count; ++i) buffer[i] = 0.0f;
        APO_CONNECTION_PROPERTY in{};
        in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
        in.u32ValidFrameCount = kMaxFrames;
        in.u32BufferFlags = BUFFER_SILENT;
        in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY out = in;
        APO_CONNECTION_PROPERTY* ins[1] = {&in};
        APO_CONNECTION_PROPERTY* outs[1] = {&out};
        isotone::audio_ring_read(ring, &cursor, drained.data(), kMaxFrames, &ring_channels);
        first.rt->APOProcess(1, ins, 1, outs);
        check(out.u32BufferFlags == BUFFER_SILENT, "a silent buffer stays flagged silent");
        const uint32_t n =
            isotone::audio_ring_read(ring, &cursor, drained.data(), kMaxFrames, &ring_channels);
        bool zeros = n == kMaxFrames;
        for (uint32_t i = 0; i < n * kChannels; ++i) zeros &= drained[i] == 0.0f;
        check(zeros, "a silent buffer reaches the ring as silence");
    }

    // ------------------------------------------------------------------
    std::printf("\nlive parameters\n");

    ui_write(shared, peaking_state(1000.0, -6.0, 1.0));
    captured = run_sine(first.rt, buffer, 1000.0, 48000);
    const double db_live = settled_db(captured, 1000.0);
    std::printf("  %-66s %+.3f dB (analytic %+.3f)\n", "level at 1 kHz after the UI wrote -6 dB",
                db_live, peaking_db(1000.0, -6.0, 1.0, 1000.0, kRate));
    check(std::abs(db_live + 6.0) < 0.01, "a block written by the UI is applied while running");

    {
        isotone::EqState poison = peaking_state(1000.0, -6.0, 1.0);
        poison.preamp_db = std::numeric_limits<double>::quiet_NaN();
        poison.bands[0].gain_db = std::numeric_limits<double>::infinity();
        ui_write(shared, poison);
        captured = run_sine(first.rt, buffer, 1000.0, 9600);
        bool finite = !captured.empty();
        for (float s : captured) finite &= std::isfinite(s);
        check(finite, "a non-finite block from the UI does not reach the output");
        ui_write(shared, peaking_state(1000.0, -6.0, 1.0));
    }

    {
        // The speaker setup travels in the same block: delay channel 0 by 62
        // samples (1.3 ms) and invert channel 1, with no bands so the samples
        // can be compared directly.
        isotone::EqState speakers;
        speakers.speakers.delay_ms[0] = 1.3;
        speakers.speakers.inverted = isotone::ChannelMask{1} << 1;
        ui_write(shared, speakers);
        captured = run_sine(first.rt, buffer, 1000.0, 48000);
        double err0 = 0.0, err1 = 0.0;
        for (size_t i = 24000; i < 48000; ++i) {
            const double now = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate);
            const double late = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i - 62) / kRate);
            err0 = (std::max)(err0, std::abs(captured[i * kChannels] - late));
            err1 = (std::max)(err1, std::abs(captured[i * kChannels + 1] + now));
        }
        check(!captured.empty() && err0 < 1e-4, "a speaker delay written by the UI delays that channel");
        check(!captured.empty() && err1 < 1e-4, "a polarity flip written by the UI inverts that channel");
        ui_write(shared, peaking_state(1000.0, -6.0, 1.0));
    }

    // ------------------------------------------------------------------
    std::printf("\nseveral instances on one endpoint\n");

    Apo second;
    check(create_apo(factory, &second), "a second instance on the same endpoint");
    check_hr(second.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
             "second Initialize opens the existing region");
    check(shared->band_count == 1 && shared->bands[0].gain_db == -6.0f,
          "opening an existing region does not re-seed it over the UI's block");

    const uint64_t first_token = ring->writer;
    check_hr(second.config->LockForProcess(1, descriptors, 1, descriptors),
             "second LockForProcess");
    check(ring->writer == first_token, "the ring stays with the instance that claimed it first");

    uint32_t index_before = ring->write_index;
    captured = run_sine(second.rt, buffer, 1000.0, 48000);
    check(ring->write_index == index_before, "the second instance does not write the ring");
    check(std::abs(settled_db(captured, 1000.0) + 6.0) < 0.01,
          "the second instance applies the UI's -6 dB block");

    check_hr(first.config->UnlockForProcess(), "first UnlockForProcess");
    check(ring->writer == 0, "unlocking releases the ring");
    index_before = ring->write_index;
    run_sine(second.rt, buffer, 1000.0, kMaxFrames);
    check(ring->writer != 0 && ring->writer != first_token && ring->write_index != index_before,
          "the second instance takes the ring over once it is free");

    check_hr(second.config->UnlockForProcess(), "second UnlockForProcess");
    first.release();
    second.release();

    // ------------------------------------------------------------------
    std::printf("\nengine restart while the UI holds the region\n");

    // Simulate a claim left behind by an audiodg process that died without
    // unlocking. A different process id marks it as stale.
    ring->writer = (uint64_t{GetCurrentProcessId() + 4} << 32) | 1u;

    Apo third;
    check(create_apo(factory, &third), "a new instance after the old ones are gone");
    check_hr(third.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
             "Initialize finds the region the UI kept alive");
    check_hr(third.config->LockForProcess(1, descriptors, 1, descriptors), "LockForProcess");
    check((ring->writer >> 32) == GetCurrentProcessId(),
          "a ring claim left by a dead engine process is taken over");
    captured = run_sine(third.rt, buffer, 1000.0, 48000);
    check(std::abs(settled_db(captured, 1000.0) + 6.0) < 0.01,
          "the new instance starts on the UI's block, not the file seed");
    check_hr(third.config->UnlockForProcess(), "UnlockForProcess");
    third.release();

    // ------------------------------------------------------------------
    std::printf("\n7.1.4, wider than the trim table\n");
    {
        constexpr UINT32 kWide = 12;
        UNCOMPRESSEDAUDIOFORMAT wide_format = format;
        wide_format.dwSamplesPerFrame = kWide;
        wide_format.dwChannelMask = 0x2D63F;   // 7.1.4: FL FR FC LFE BL BR SL SR TFL TFR TBL TBR
        IAudioMediaType* wide_media = nullptr;
        check_hr(CreateAudioMediaTypeFromUncompressedAudioFormat(&wide_format, &wide_media),
                 "12-channel media type");
        float* wide_buffer = static_cast<float*>(
            _aligned_malloc(static_cast<size_t>(kMaxFrames) * kWide * sizeof(float), 32));

        APO_CONNECTION_DESCRIPTOR wide_descriptor = descriptor;
        wide_descriptor.pBuffer = reinterpret_cast<UINT_PTR>(wide_buffer);
        wide_descriptor.pFormat = wide_media;
        APO_CONNECTION_DESCRIPTOR* wide_descriptors[1] = {&wide_descriptor};

        isotone::EqState s = peaking_state(1000.0, -12.0, 1.0);
        s.bands[0].channels = isotone::ChannelMask{1} << 10;   // top back left only
        // An all-channel band as well: with no band on channels 0-7, walking the
        // buffer with the wrong stride would be a pass-through and hide the bug.
        s.bands.push_back(peaking_state(1000.0, -3.0, 1.0).bands[0]);
        s.bands[1].id = 2;
        ui_write(shared, s);

        Apo wide;
        check(create_apo(factory, &wide) && wide_media != nullptr && wide_buffer != nullptr,
              "an instance for a 12-channel stream");
        check_hr(wide.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)), "Initialize");
        check_hr(wide.config->LockForProcess(1, wide_descriptors, 1, wide_descriptors),
                 "LockForProcess at 12 channels");
        check(shared->hdr.channels == kWide && ring->channels == isotone::kMaxChannels,
              "the header says 12 channels and the ring stores the first 8");
        check(shared->hdr.speaker_mask == wide_format.dwChannelMask,
              "the header carries the stream's speaker mask");

        // Channel c carries amplitude (c + 1) / 16, so a channel read from the
        // wrong slot shows up as a level error.
        isotone::AudioRingCursor wide_cursor;
        isotone::audio_ring_read(ring, &wide_cursor, drained.data(), kMaxFrames, &ring_channels);
        std::vector<float> out;
        bool ring_ok = true;
        const size_t total = 48000;
        for (size_t position = 0; position < total; position += kMaxFrames) {
            for (UINT32 i = 0; i < kMaxFrames; ++i) {
                const double v = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(position + i) / kRate);
                for (UINT32 c = 0; c < kWide; ++c) {
                    wide_buffer[i * kWide + c] = static_cast<float>(v * (c + 1) / 16.0);
                }
            }
            APO_CONNECTION_PROPERTY in{};
            in.pBuffer = reinterpret_cast<UINT_PTR>(wide_buffer);
            in.u32ValidFrameCount = kMaxFrames;
            in.u32BufferFlags = BUFFER_VALID;
            in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY o = in;
            APO_CONNECTION_PROPERTY* ins[1] = {&in};
            APO_CONNECTION_PROPERTY* outs[1] = {&o};
            wide.rt->APOProcess(1, ins, 1, outs);
            out.insert(out.end(), wide_buffer, wide_buffer + static_cast<size_t>(kMaxFrames) * kWide);

            const uint32_t n =
                isotone::audio_ring_read(ring, &wide_cursor, drained.data(), kMaxFrames, &ring_channels);
            ring_ok &= n == kMaxFrames && ring_channels == isotone::kMaxChannels;
            for (UINT32 i = 0; i < n && ring_ok; ++i) {
                ring_ok = std::memcmp(drained.data() + static_cast<size_t>(i) * isotone::kMaxChannels,
                                      wide_buffer + static_cast<size_t>(i) * kWide,
                                      isotone::kMaxChannels * sizeof(float)) == 0;
            }
        }
        check(ring_ok, "the ring holds channels 0-7 of every output frame, bit for bit");

        const size_t frames = out.size() / kWide;
        const size_t skip = frames / 2;
        const auto level = [&](UINT32 c) {
            return 20.0 * std::log10(amplitude_at(out.data() + skip * kWide + c, frames - skip,
                                                  kWide, 1000.0, kRate) * 16.0 / (c + 1));
        };
        std::printf("  %-66s %+.3f dB\n", "level on channel 10 (-12 dB band plus -3 dB on all)", level(10));
        std::printf("  %-66s %+.3f dB\n", "level on channel 11", level(11));
        std::printf("  %-66s %+.3f dB\n", "level on channel 2", level(2));
        check(std::abs(level(10) + 15.0) < 0.01, "channel 10 gets its own band and the shared one");
        bool others = true;
        for (UINT32 c = 0; c < kWide; ++c) {
            if (c != 10) others &= std::abs(level(c) + 3.0) < 0.01;
        }
        check(others, "the other eleven channels get only the shared -3 dB band");

        check_hr(wide.config->UnlockForProcess(), "UnlockForProcess");
        wide.release();
        _aligned_free(wide_buffer);
        if (wide_media) wide_media->Release();
    }

    _aligned_free(buffer);
    media->Release();
    factory->Release();

    check(can_unload() == S_OK, "DllCanUnloadNow says yes again after release");

    ui.close();
    isotone::win::SharedMapping probe;
    check(probe.open(name) == ERROR_FILE_NOT_FOUND,
          "the region is gone once the engine and the UI have both let go");

    FreeLibrary(module);
    CoUninitialize();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
