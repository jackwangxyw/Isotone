// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Hosts IsoAPO.dll in this process and drives the real COM lifecycle: class
// factory, Initialize, LockForProcess, APOProcess, UnlockForProcess. Audio is
// pushed through it and measured.
//
// The point is to find out whether the APO works before letting audiodg.exe load
// it. A fault here is a failed exit code; the same fault inside audiodg takes
// down every sound on the machine until the audio service restarts.
//
// Registration is not needed: the DLL is loaded directly and DllGetClassObject
// is called by hand, so this touches no registry key and no audio device.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

void check_hr(HRESULT hr, const char* what) {
    const bool ok = SUCCEEDED(hr);
    if (ok) {
        std::printf("  %-58s ok\n", what);
    } else {
        std::printf("  %-58s FAIL (0x%08lx)\n", what, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// Analytic response of the peaking filter the APO defaults to, so the test
// compares against the maths rather than against a guessed tolerance. An RBJ
// bell is wide: a Q of 1 centred at 1 kHz still pulls 0.16 dB at 100 Hz, which
// is not "untouched" and should not be asserted as such.
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

}  // namespace

int main(int argc, char** argv) {
    const std::wstring dll = argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                                      : L"IsoAPO.dll";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::fprintf(stderr, "CoInitializeEx failed\n");
        return 2;
    }

    std::printf("IsoAPO self test\n");

    HMODULE module = LoadLibraryW(dll.c_str());
    check(module != nullptr, "LoadLibrary IsoAPO.dll");
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
    if (get_class_object == nullptr) {
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

    IAudioProcessingObject* apo = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                 reinterpret_cast<void**>(&apo));
    check_hr(hr, "CreateInstance for IAudioProcessingObject");
    if (FAILED(hr)) return 2;

    check(can_unload() == S_FALSE, "DllCanUnloadNow says no while an instance is live");

    IAudioProcessingObjectConfiguration* config = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioSystemEffects* effects = nullptr;
    check_hr(apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                 reinterpret_cast<void**>(&config)),
             "QueryInterface IAudioProcessingObjectConfiguration");
    check_hr(apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                 reinterpret_cast<void**>(&rt)),
             "QueryInterface IAudioProcessingObjectRT");
    check_hr(apo->QueryInterface(__uuidof(IAudioSystemEffects),
                                 reinterpret_cast<void**>(&effects)),
             "QueryInterface IAudioSystemEffects");
    if (config == nullptr || rt == nullptr) return 2;

    HNSTIME latency = -1;
    check_hr(apo->GetLatency(&latency), "GetLatency");
    check(latency == 0, "latency is zero, as a biquad cascade should be");

    check_hr(apo->Initialize(0, nullptr), "Initialize");

    // Build the format the audio engine would negotiate: 48 kHz, stereo, float32.
    constexpr double kRate = 48000.0;
    constexpr UINT32 kChannels = 2;
    constexpr UINT32 kMaxFrames = 1024;

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
    check_hr(config->LockForProcess(1, descriptors, 1, descriptors), "LockForProcess");

    // Push a 1 kHz sine through in blocks, in place, as the engine does.
    const double freq = 1000.0;
    const size_t total_frames = 48000;   // one second
    std::vector<float> captured;
    captured.reserve(total_frames * kChannels);

    size_t position = 0;
    while (position < total_frames) {
        const UINT32 frames =
            static_cast<UINT32>((total_frames - position) < kMaxFrames ? (total_frames - position)
                                                                       : kMaxFrames);
        for (UINT32 i = 0; i < frames; ++i) {
            const double t = static_cast<double>(position + i);
            const float v = static_cast<float>(std::sin(2.0 * kPi * freq * t / kRate));
            for (UINT32 c = 0; c < kChannels; ++c) {
                buffer[i * kChannels + c] = v;
            }
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
            check(false, "APOProcess returned the frame count it was given");
            break;
        }
        captured.insert(captured.end(), buffer, buffer + static_cast<size_t>(frames) * kChannels);
        position += frames;
    }

    check(captured.size() == total_frames * kChannels, "one second of audio came back");

    // Skip the first half second so the filter is well settled, then measure.
    const size_t skip = total_frames / 2;
    const size_t window = total_frames - skip;
    const double at_1k =
        amplitude_at(captured.data() + skip * kChannels, window, kChannels, 1000.0, kRate);
    const double db_1k = 20.0 * std::log10(at_1k);
    std::printf("  %-58s %+.3f dB (analytic %+.3f)\n", "measured level at 1 kHz", db_1k,
                peaking_db(1000.0, -12.0, 1.0, 1000.0, kRate));
    check(std::abs(db_1k + 12.0) < 0.01, "the -12 dB dip at 1 kHz is present");

    // A frequency far from the band should be untouched.
    std::vector<float> off_band;
    off_band.reserve(total_frames * kChannels);
    position = 0;
    while (position < total_frames) {
        const UINT32 frames =
            static_cast<UINT32>((total_frames - position) < kMaxFrames ? (total_frames - position)
                                                                       : kMaxFrames);
        for (UINT32 i = 0; i < frames; ++i) {
            const double t = static_cast<double>(position + i);
            const float v = static_cast<float>(std::sin(2.0 * kPi * 100.0 * t / kRate));
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
        off_band.insert(off_band.end(), buffer, buffer + static_cast<size_t>(frames) * kChannels);
        position += frames;
    }
    const double db_100 = 20.0 * std::log10(amplitude_at(
        off_band.data() + skip * kChannels, window, kChannels, 100.0, kRate));
    const double expect_100 = peaking_db(1000.0, -12.0, 1.0, 100.0, kRate);
    std::printf("  %-58s %+.3f dB (analytic %+.3f)\n", "measured level at 100 Hz", db_100,
                expect_100);
    check(std::abs(db_100 - expect_100) < 0.01, "100 Hz matches the analytic skirt of the band");

    // A silent buffer must come back flagged silent, or some drivers misbehave.
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
        rt->APOProcess(1, ins, 1, outs);
        check(out.u32BufferFlags == BUFFER_SILENT, "a silent buffer stays flagged silent");
    }

    check_hr(config->UnlockForProcess(), "UnlockForProcess");

    _aligned_free(buffer);
    media->Release();
    if (effects) effects->Release();
    rt->Release();
    config->Release();
    apo->Release();
    factory->Release();

    check(can_unload() == S_OK, "DllCanUnloadNow says yes again after release");

    FreeLibrary(module);
    CoUninitialize();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
