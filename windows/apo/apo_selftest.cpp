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
// is called by hand, and no audio device is touched. It loads
// IsoAPO-selftest.dll, the build of the same sources that puts the region in
// Local\ rather than Global\, because this process cannot create Global\
// objects without elevation, and that reads child-APO records from
// HKCU\Software\IsoAPO-selftest, which the child test writes and deletes.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <aclapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <psapi.h>
#include <sddl.h>
#include <winver.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "persisted_state.h"
#include "shared_mapping.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr UINT32 kChannels = 2;
constexpr UINT32 kMaxFrames = 1024;

int g_failures = 0;

// The FileVersion in a binary's VERSIONINFO, as Settings, About reads IsoAPO's
// (ui/backend/diagnostics.cpp). Empty when the binary carries no resource at
// all, which is what every build before 0.1.0 produced.
std::string file_version(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
        info == nullptr || length < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%u.%u.%u", HIWORD(info->dwFileVersionMS),
                  LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS));
    return text;
}

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
// returned the wrong frame count. With `out_buffer` the output goes there
// instead, pre-filled with garbage, as an engine with separate buffers would.
std::vector<float> run_sine(IAudioProcessingObjectRT* rt, float* buffer, double freq,
                            size_t total_frames,
                            const std::function<void(UINT32)>& after = nullptr,
                            float* out_buffer = nullptr) {
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
        if (out_buffer != nullptr) {
            for (size_t i = 0; i < static_cast<size_t>(frames) * kChannels; ++i) out_buffer[i] = 7.0f;
            out.pBuffer = reinterpret_cast<UINT_PTR>(out_buffer);
        }
        APO_CONNECTION_PROPERTY* ins[1] = {&in};
        APO_CONNECTION_PROPERTY* outs[1] = {&out};
        rt->APOProcess(1, ins, 1, outs);
        if (out.u32ValidFrameCount != frames) {
            return {};
        }
        const float* result = out_buffer != nullptr ? out_buffer : buffer;
        captured.insert(captured.end(), result, result + static_cast<size_t>(frames) * kChannels);
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

// A stand-in for the vendor APO an install replaces. It scales the audio by
// kChildGain and counts every call, so the test can see IsoAPO forward each step
// and process what the child wrote. Registered in this process only, with
// CoRegisterClassObject.
constexpr double kChildGain = 0.5;
constexpr HNSTIME kChildLatency = 12345;
// {5C1D0E2A-4B7F-4E11-9A31-6D2B8E4017C3}
constexpr CLSID kTestChildClsid = {0x5c1d0e2a, 0x4b7f, 0x4e11, {0x9a, 0x31, 0x6d, 0x2b, 0x8e, 0x40, 0x17, 0xc3}};

struct ChildProbe {
    int live = 0, created = 0, initialized = 0, format_calls = 0, locks = 0, unlocks = 0, processes = 0, resets = 0;
    bool fail_initialize = false;
    bool fail_lock = false;
    bool skip_output_flags = false;   // leave the output buffer flags as they were
    bool host_isoapo = false;         // a wrapper whose own child is IsoAPO
    HRESULT nested_init = S_OK;       // what the IsoAPO it hosts said to Initialize
    int refuse_formats = 0;   // refuse this many format probes first
    bool refuse_locked = false;   // refuse every probe while this child is locked
    std::wstring endpoint;
    CLSID init_clsid{};
};
ChildProbe g_child;
IClassFactory* g_isoapo_factory = nullptr;

class TestChild : public IAudioProcessingObject,
                  public IAudioProcessingObjectRT,
                  public IAudioProcessingObjectConfiguration {
public:
    TestChild() { ++g_child.live; ++g_child.created; }
    ~TestChild() { --g_child.live; }

    HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioProcessingObject)) {
            *ppv = static_cast<IAudioProcessingObject*>(this);
        } else if (iid == __uuidof(IAudioProcessingObjectRT)) {
            *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        } else if (iid == __uuidof(IAudioProcessingObjectConfiguration)) {
            *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG __stdcall AddRef() override { return ++refs_; }
    ULONG __stdcall Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    HRESULT __stdcall Reset() override {
        ++g_child.resets;
        return S_OK;
    }
    HRESULT __stdcall GetLatency(HNSTIME* time) override {
        *time = kChildLatency;
        return S_OK;
    }
    HRESULT __stdcall GetRegistrationProperties(APO_REG_PROPERTIES**) override { return E_NOTIMPL; }
    HRESULT __stdcall Initialize(UINT32 size, BYTE* data) override {
        ++g_child.initialized;
        if (g_child.fail_initialize) return E_FAIL;
        if (size < sizeof(APOInitSystemEffects) || data == nullptr) return E_INVALIDARG;
        // A wrapper that hosts IsoAPO, as its own install record says. Bounded
        // here, so a missing guard fails the check instead of the stack.
        if (g_child.host_isoapo && g_child.created <= 3) {
            Apo nested;
            if (create_apo(g_isoapo_factory, &nested)) g_child.nested_init = nested.apo->Initialize(size, data);
            nested.release();
        }
        const auto* init = reinterpret_cast<APOInitSystemEffects*>(data);
        g_child.init_clsid = init->APOInit.clsid;
        static constexpr PROPERTYKEY kEndpointGuid = {
            {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(init->pAPOEndpointProperties->GetValue(kEndpointGuid, &var)) && var.vt == VT_LPWSTR) {
            g_child.endpoint = var.pwszVal;
        }
        PropVariantClear(&var);
        return S_OK;
    }
    HRESULT __stdcall IsInputFormatSupported(IAudioMediaType*, IAudioMediaType* requested,
                                             IAudioMediaType** supported) override {
        ++g_child.format_calls;
        if (g_child.refuse_locked && g_child.locks > g_child.unlocks) {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
        if (g_child.refuse_formats > 0) {
            --g_child.refuse_formats;
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
        requested->AddRef();
        *supported = requested;
        return S_OK;
    }
    HRESULT __stdcall IsOutputFormatSupported(IAudioMediaType*, IAudioMediaType* requested,
                                              IAudioMediaType** supported) override {
        requested->AddRef();
        *supported = requested;
        return S_OK;
    }
    HRESULT __stdcall GetInputChannelCount(UINT32* count) override {
        *count = kChannels;
        return S_OK;
    }

    void __stdcall APOProcess(UINT32, APO_CONNECTION_PROPERTY** in, UINT32, APO_CONNECTION_PROPERTY** out) override {
        ++g_child.processes;
        const auto* src = reinterpret_cast<const float*>(in[0]->pBuffer);
        auto* dst = reinterpret_cast<float*>(out[0]->pBuffer);
        const size_t samples = static_cast<size_t>(in[0]->u32ValidFrameCount) * kChannels;
        for (size_t i = 0; i < samples; ++i) dst[i] = static_cast<float>(src[i] * kChildGain);
        out[0]->u32ValidFrameCount = in[0]->u32ValidFrameCount;
        if (!g_child.skip_output_flags) out[0]->u32BufferFlags = in[0]->u32BufferFlags;
    }
    UINT32 __stdcall CalcInputFrames(UINT32 frames) override { return frames; }
    UINT32 __stdcall CalcOutputFrames(UINT32 frames) override { return frames; }

    HRESULT __stdcall LockForProcess(UINT32, APO_CONNECTION_DESCRIPTOR**, UINT32,
                                     APO_CONNECTION_DESCRIPTOR**) override {
        ++g_child.locks;
        return g_child.fail_lock ? E_FAIL : S_OK;
    }
    HRESULT __stdcall UnlockForProcess() override {
        ++g_child.unlocks;
        return S_OK;
    }

private:
    ULONG refs_ = 1;
};

class TestChildFactory : public IClassFactory {
public:
    HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 2; }    // static lifetime
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall CreateInstance(IUnknown* outer, const IID& iid, void** ppv) override {
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        auto* child = new TestChild();
        const HRESULT hr = child->QueryInterface(iid, ppv);
        child->Release();
        return hr;
    }
    HRESULT __stdcall LockServer(BOOL) override { return S_OK; }
};

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

    // The version resource, because Settings, About shows the engine's version
    // from it and a DLL built without one reports nothing at all. Both builds
    // come from isoapo_dll(), so the shipped IsoAPO.dll is checked by name too
    // when it is beside this one.
    {
        const std::string want = ISOTONE_VERSION;
        const std::string got = file_version(dll);
        check(got == want, "the DLL under test carries the project version");
        if (got != want) {
            std::fprintf(stderr, "  FileVersion = '%s', expected '%s'\n", got.c_str(), want.c_str());
        }
        const std::string shipped = file_version(L"IsoAPO.dll");
        check(shipped == want, "IsoAPO.dll beside it carries the same version");
    }

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
    g_isoapo_factory = factory;

    void* ignored = nullptr;
    check(get_class_object(bogus, __uuidof(IClassFactory), &ignored) ==
              CLASS_E_CLASSNOTAVAILABLE,
          "an unknown CLSID is refused");

    // The factory's code lives in the DLL, so a held factory must keep it loaded.
    check(can_unload() == S_FALSE, "DllCanUnloadNow says no while a class factory is held");

    // An instance answers for the class it was created as. Every instance used
    // to be built with the post-mix properties, so a pre-mix one reported the
    // post-mix CLSID to whoever asked what it was.
    // {F1DFFD14-9A30-45C5-BAB2-C820C7EC718F}
    const CLSID pre_mix = {0xf1dffd14, 0x9a30, 0x45c5,
                           {0xba, 0xb2, 0xc8, 0x20, 0xc7, 0xec, 0x71, 0x8f}};
    const auto registered_clsid = [&](const CLSID& asked, CLSID* got) {
        IClassFactory* f = nullptr;
        if (FAILED(get_class_object(asked, __uuidof(IClassFactory), reinterpret_cast<void**>(&f)))) return false;
        Apo instance;
        const bool made = create_apo(f, &instance);
        APO_REG_PROPERTIES* properties = nullptr;
        const bool read = made && SUCCEEDED(instance.apo->GetRegistrationProperties(&properties)) &&
                          properties != nullptr;
        if (read) *got = properties->clsid;
        if (properties != nullptr) CoTaskMemFree(properties);
        instance.release();
        f->Release();
        return read;
    };
    CLSID reported{};
    check(registered_clsid(post_mix, &reported) && reported == post_mix,
          "a post-mix instance reports the post-mix CLSID");
    check(registered_clsid(pre_mix, &reported) && reported == pre_mix,
          "a pre-mix instance reports the pre-mix CLSID");

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

    {
        // IsoAPO passes PKEY_AudioEndpoint_GUID, braced upper case; the UI and
        // the tools may have a bare GUID or the full IMMDevice::GetId string.
        // Every form names the region and the saved file IsoAPO uses, and the
        // braced form's names are the ones existing regions and files have.
        std::wstring lower = endpoint_upper;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const std::wstring bare_upper = std::wstring(endpoint_upper).substr(1, 36);
        const std::wstring expected_name = L"Local\\IsoAPO." + lower;
        const std::wstring expected_file = L"C:\\dir\\" + lower + L".bin";
        bool same = name == expected_name &&
                    isotone::win::persisted_state_path(L"C:\\dir", endpoint_upper) == expected_file;
        for (const std::wstring& form : {bare_upper, lower.substr(1, 36), L"{0.0.0.00000000}." + lower,
                                         L"{0.0.1.00000000}." + std::wstring(endpoint_upper),
                                         L" \t" + std::wstring(endpoint_upper) + L"\r\n",
                                         L"  {0.0.0.00000000}.{" + bare_upper + L"} "}) {
            same &= isotone::win::mapping_name(L"Local\\", form) == expected_name &&
                    isotone::win::persisted_state_path(L"C:\\dir", form) == expected_file;
        }
        check(same, "braced, bare and full device ID forms of the GUID name the same region and file");
        bool refused = true;
        for (const std::wstring& form : {std::wstring(L"not-a-guid"), std::wstring(L"{0.0.0.00000000}"),
                                         std::wstring(L"{0.0.0.00000000}.") + bare_upper,
                                         bare_upper + L"0", std::wstring(L"")}) {
            refused &= isotone::win::mapping_name(L"Local\\", form).empty() &&
                       isotone::win::persisted_state_path(L"C:\\dir", form).empty();
        }
        isotone::win::SharedMapping nameless;
        check(refused && nameless.create_or_open(L"") == ERROR_INVALID_NAME && nameless.open(L"") == ERROR_INVALID_NAME,
              "text that is not an endpoint GUID names no region and no file");
    }

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
    isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &cursor, drained.data(), kMaxFrames, &ring_channels);

    // Drain the ring after every block, as the UI's timer would, and demand the
    // ring hold exactly what the APO output.
    bool ring_matches = true;
    size_t ring_frames = 0;
    const auto drain_and_compare = [&](UINT32 frames) {
        const uint32_t n =
            isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &cursor, drained.data(), kMaxFrames, &ring_channels);
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

    // Silent buffers go through the filters, so the band's tail after the sine
    // plays out and is flagged audible; once it has died away the buffer is
    // flagged silent again, which some drivers need, and the ring shows silence.
    {
        isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &cursor, drained.data(), kMaxFrames, &ring_channels);
        bool first_audible = false, became_silent = false, zeros = false;
        for (int call = 0; call < 200 && !became_silent; ++call) {
            for (size_t i = 0; i < sample_count; ++i) buffer[i] = 0.5f;   // garbage: a silent buffer is undefined
            APO_CONNECTION_PROPERTY in{};
            in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
            in.u32ValidFrameCount = kMaxFrames;
            in.u32BufferFlags = BUFFER_SILENT;
            in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY out = in;
            APO_CONNECTION_PROPERTY* ins[1] = {&in};
            APO_CONNECTION_PROPERTY* outs[1] = {&out};
            first.rt->APOProcess(1, ins, 1, outs);
            if (call == 0) first_audible = out.u32BufferFlags == BUFFER_VALID;
            const uint32_t n = isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &cursor, drained.data(),
                                                        kMaxFrames, &ring_channels);
            if (out.u32BufferFlags == BUFFER_SILENT) {
                became_silent = true;
                zeros = n == kMaxFrames;
                for (uint32_t i = 0; i < n * kChannels; ++i) zeros &= drained[i] == 0.0f;
            }
        }
        check(first_audible, "the first silent buffer after audio carries the filter's tail");
        check(became_silent, "silent buffers are flagged silent again once the tail is gone");
        check(zeros, "a silent buffer reaches the ring as silence");
    }

    // Lip sync delays everything by 50 ms. The last 50 ms of the sine must play
    // out over the silent buffers that follow, and must not be left in the delay
    // line to play before the next sound.
    {
        isotone::EqState delayed;
        delayed.speakers.lip_sync_ms = 50.0;   // 2400 samples
        ui_write(shared, delayed);
        run_sine(first.rt, buffer, 1000.0, 48000);
        double tail_peak = 0.0;
        for (int call = 0; call < 4; ++call) {   // 4096 frames of silence
            APO_CONNECTION_PROPERTY in{};
            in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
            in.u32ValidFrameCount = kMaxFrames;
            in.u32BufferFlags = BUFFER_SILENT;
            in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY out = in;
            APO_CONNECTION_PROPERTY* ins[1] = {&in};
            APO_CONNECTION_PROPERTY* outs[1] = {&out};
            first.rt->APOProcess(1, ins, 1, outs);
            for (size_t i = 0; i < sample_count && call < 2; ++i) tail_peak = (std::max)(tail_peak, double(std::abs(buffer[i])));
        }
        const std::vector<float> resumed = run_sine(first.rt, buffer, 1000.0, 4800);
        double stale = 0.0;
        for (size_t i = 0; i < 2300 * kChannels && i < resumed.size(); ++i) stale = (std::max)(stale, double(std::abs(resumed[i])));
        std::printf("  %-66s %.3f / %.2e\n", "delayed tail peak in silence / stale audio after resume", tail_peak, stale);
        check(tail_peak > 0.9, "a delayed tail plays out over the silent buffers that follow the sound");
        check(stale < 1e-6, "nothing stale is left in the delay line for the next sound");

        // A stream stopped with no silence after it: Reset must empty the delay
        // line, or the next start plays the old tail.
        run_sine(first.rt, buffer, 1000.0, 48000);
        check_hr(first.apo->Reset(), "Reset");
        const std::vector<float> after_reset = run_sine(first.rt, buffer, 1000.0, 4800);
        double leftover = 0.0;
        for (size_t i = 0; i < 2300 * kChannels && i < after_reset.size(); ++i) leftover = (std::max)(leftover, double(std::abs(after_reset[i])));
        check(!after_reset.empty() && leftover < 1e-6, "Reset empties the delay line");
        ui_write(shared, peaking_state(1000.0, -6.0, 1.0));
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
        // Upstream sizes by the output's max frame count when the input's is 0.
        Apo zero;
        create_apo(factory, &zero);
        zero.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        APO_CONNECTION_DESCRIPTOR no_max = descriptor;
        no_max.u32MaxFrameCount = 0;
        APO_CONNECTION_DESCRIPTOR* no_max_in[1] = {&no_max};
        const HRESULT locked = zero.config->LockForProcess(1, no_max_in, 1, descriptors);
        const std::vector<float> out = SUCCEEDED(locked) ? run_sine(zero.rt, buffer, 1000.0, 48000) : std::vector<float>{};
        check(SUCCEEDED(locked) && !out.empty() && std::abs(settled_db(out, 1000.0) + 6.0) < 0.01,
              "an input with no max frame count is processed at the output's");
        if (SUCCEEDED(locked)) zero.config->UnlockForProcess();
        zero.release();
    }

    {
        // Negotiated at 480 frames, handed 1024 valid frames a call.
        Apo capped;
        create_apo(factory, &capped);
        capped.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        APO_CONNECTION_DESCRIPTOR small_max = descriptor;
        small_max.u32MaxFrameCount = 480;
        APO_CONNECTION_DESCRIPTOR* small_max_in[1] = {&small_max};
        const HRESULT locked = capped.config->LockForProcess(1, small_max_in, 1, small_max_in);
        const std::vector<float> out = SUCCEEDED(locked) ? run_sine(capped.rt, buffer, 1000.0, 48000) : std::vector<float>{};
        const double level = out.empty() ? 0.0 : settled_db(out, 1000.0);
        std::printf("  %-66s %+.3f dB\n", "level with 1024 frames a call locked at 480", level);
        check(SUCCEEDED(locked) && std::abs(level + 6.0) < 0.01,
              "a buffer longer than the negotiated maximum is processed in full");
        if (SUCCEEDED(locked)) capped.config->UnlockForProcess();
        capped.release();
    }

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
    std::printf("\nformat negotiation and locking\n");
    {
        check(first.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)) == APOERR_ALREADY_INITIALIZED,
              "Initialize while locked is refused");

        // Alone, IsoAPO cannot change the channel count: stereo in, 5.1 out must
        // be answered with a 5.1 input suggestion, not accepted.
        UNCOMPRESSEDAUDIOFORMAT six = format;
        six.dwSamplesPerFrame = 6;
        six.dwChannelMask = 0x60F;
        IAudioMediaType* six_media = nullptr;
        CreateAudioMediaTypeFromUncompressedAudioFormat(&six, &six_media);
        Apo probe;
        create_apo(factory, &probe);
        probe.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        IAudioMediaType* suggested = nullptr;
        const HRESULT hr_mismatch = probe.apo->IsInputFormatSupported(six_media, media, &suggested);
        UNCOMPRESSEDAUDIOFORMAT suggestion{};
        const bool suggested_six = suggested != nullptr && SUCCEEDED(suggested->GetUncompressedAudioFormat(&suggestion)) &&
                                   suggestion.dwSamplesPerFrame == 6;
        check(hr_mismatch == S_FALSE && suggested_six,
              "a channel-count change is answered with a suggestion in the output's layout");
        if (suggested) suggested->Release();
        IAudioMediaType* same = nullptr;
        check(probe.apo->IsInputFormatSupported(media, media, &same) == S_OK && same != nullptr,
              "a matching format is accepted");
        if (same) same->Release();
        probe.release();
        if (six_media) six_media->Release();
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
    // unlocking, first from another process id, then from the id this process
    // has now: Windows reuses process ids, so the new audiodg can get the dead
    // one's.
    for (const uint64_t stale : {(uint64_t{GetCurrentProcessId() + 4} << 32) | 1u,
                                 (uint64_t{GetCurrentProcessId()} << 32) | 1u}) {
        ring->writer = stale;

        Apo third;
        check(create_apo(factory, &third), "a new instance after the old ones are gone");
        check_hr(third.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
                 "Initialize finds the region the UI kept alive");
        check_hr(third.config->LockForProcess(1, descriptors, 1, descriptors), "LockForProcess");
        run_sine(third.rt, buffer, 1000.0, size_t{kMaxFrames} * (isotone::kRingStaleClaims + 2));
        check(ring->writer != stale && ring->writer != 0,
              (stale >> 32) == GetCurrentProcessId()
                  ? "a stale ring claim carrying this process's id is taken over too"
                  : "a ring claim left by a dead engine process is taken over once its writes have stopped");
        captured = run_sine(third.rt, buffer, 1000.0, 48000);
        check(std::abs(settled_db(captured, 1000.0) + 6.0) < 0.01,
              "the new instance starts on the UI's block, not the file seed");
        check_hr(third.config->UnlockForProcess(), "UnlockForProcess");
        third.release();
    }

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
        isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &wide_cursor, drained.data(), kMaxFrames, &ring_channels);
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
                isotone::audio_ring_read(ring, isotone::kRingCapacityFrames, &wide_cursor, drained.data(), kMaxFrames, &ring_channels);
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

    // ------------------------------------------------------------------
    std::printf("\na state written for 7.1 on other layouts\n");
    {
        // 7.1 surround is L R C LFE RL RR SL SR. A -12 dB band on SL, a -6 dB
        // trim and polarity on SR, a 1 ms delay on SL, C muted, each by 7.1's
        // index. On 5.1 surround (L R C LFE SL SR) SL and SR are channels 4 and
        // 5; read by index, channels 6 and 7 do not exist and nothing would play.
        isotone::EqState s71 = peaking_state(1000.0, -12.0, 1.0);
        s71.layout_channels = 8;
        s71.layout_speaker_mask = 0x63F;
        s71.bands[0].channels = isotone::ChannelMask{1} << 6;
        s71.channel_gain_db[7] = -6.0;
        s71.speakers.inverted = isotone::ChannelMask{1} << 7;
        s71.speakers.delay_ms[6] = 1.0;
        s71.speakers.muted = isotone::ChannelMask{1} << 2;

        constexpr UINT32 kSix = 6;
        UNCOMPRESSEDAUDIOFORMAT six_format = format;
        six_format.dwSamplesPerFrame = kSix;
        six_format.dwChannelMask = 0x60F;
        IAudioMediaType* six_media = nullptr;
        CreateAudioMediaTypeFromUncompressedAudioFormat(&six_format, &six_media);
        std::vector<float> six(size_t{kMaxFrames} * kSix);
        APO_CONNECTION_DESCRIPTOR six_d = descriptor;
        six_d.pBuffer = reinterpret_cast<UINT_PTR>(six.data());
        six_d.pFormat = six_media;
        APO_CONNECTION_DESCRIPTOR* six_ds[1] = {&six_d};

        // One second of a 0.5 amplitude 1 kHz sine on every channel.
        const auto run_six = [&](IAudioProcessingObjectRT* rt) {
            std::vector<float> out;
            for (size_t position = 0; position < 48000; position += kMaxFrames) {
                for (UINT32 i = 0; i < kMaxFrames; ++i) {
                    const float v = static_cast<float>(
                        0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(position + i) / kRate));
                    for (UINT32 c = 0; c < kSix; ++c) six[i * kSix + c] = v;
                }
                APO_CONNECTION_PROPERTY in{};
                in.pBuffer = reinterpret_cast<UINT_PTR>(six.data());
                in.u32ValidFrameCount = kMaxFrames;
                in.u32BufferFlags = BUFFER_VALID;
                in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
                APO_CONNECTION_PROPERTY o = in;
                APO_CONNECTION_PROPERTY* ins[1] = {&in};
                APO_CONNECTION_PROPERTY* outs[1] = {&o};
                rt->APOProcess(1, ins, 1, outs);
                out.insert(out.end(), six.begin(), six.end());
            }
            return out;
        };
        const auto level_of = [](const std::vector<float>& out, UINT32 channels, UINT32 c) {
            const size_t frames = out.size() / channels;
            const size_t skip = frames / 2;
            return 20.0 * std::log10(std::max(
                amplitude_at(out.data() + skip * channels + c, frames - skip, channels, 1000.0, kRate) / 0.5, 1e-10));
        };
        // The sign of a channel against channel 0, which nothing inverts.
        const auto polarity_of = [](const std::vector<float>& out, UINT32 channels, UINT32 c) {
            double dot = 0.0;
            for (size_t f = out.size() / channels / 2; f < out.size() / channels; ++f) {
                dot += static_cast<double>(out[f * channels + c]) * out[f * channels];
            }
            return dot < 0.0 ? -1 : 1;
        };
        // `from_start`: the stream has just locked, so the delay lines start silent.
        const auto on_six = [&](const std::vector<float>& out, double band_db, bool from_start) {
            bool ok = out.size() == size_t{48000 / kMaxFrames + 1} * kMaxFrames * kSix;
            const double sl_band = peaking_db(1000.0, band_db, 1.0, 1000.0, kRate);
            for (UINT32 c = 0; c < kSix && ok; ++c) {
                const double expected = c == 2 ? -200.0 : c == 4 ? sl_band : c == 5 ? -6.0 : 0.0;
                const double level = level_of(out, kSix, c);
                std::printf("  %-66s %+.3f dB\n", ("level on channel " + std::to_string(c)).c_str(), level);
                ok = c == 2 ? level < -120.0 : std::abs(level - expected) < 0.01;
            }
            // 1 ms is 48 samples at 48 kHz: SL is silent until then, L is not.
            bool delayed = true;
            for (size_t f = 0; f < 48 && ok && from_start; ++f) delayed &= out[f * kSix + 4] == 0.0f;
            delayed &= !from_start || (out[50 * kSix + 4] != 0.0f && out[10 * kSix] != 0.0f);
            return ok && delayed && polarity_of(out, kSix, 5) == -1 && polarity_of(out, kSix, 4) == 1;
        };

        Apo apo;
        create_apo(factory, &apo);
        apo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        ui_write(shared, s71);
        const HRESULT locked = apo.config->LockForProcess(1, six_ds, 1, six_ds);
        check(SUCCEEDED(locked) && six_media != nullptr && on_six(run_six(apo.rt), -12.0, true),
              "on 5.1 each value locks onto its speaker: SL band and delay, SR trim and polarity, C mute");
        isotone::EqState s71_nine = s71;
        s71_nine.bands[0].gain_db = -9.0;
        ui_write(shared, s71_nine);
        run_six(apo.rt);   // the band's change settles in this second
        check(SUCCEEDED(locked) && on_six(run_six(apo.rt), -9.0, false),
              "a block written for 7.1 while playing 5.1 lands the same way");
        if (SUCCEEDED(locked)) apo.config->UnlockForProcess();
        apo.release();
        if (six_media) six_media->Release();

        // Stereo has none of SL, SR or C: the band plays nowhere, which an empty
        // mask read as all channels would not, and the trim, polarity and mute go.
        Apo stereo;
        create_apo(factory, &stereo);
        stereo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        ui_write(shared, s71);
        const HRESULT stereo_locked = stereo.config->LockForProcess(1, descriptors, 1, descriptors);
        const std::vector<float> out = SUCCEEDED(stereo_locked) ? run_sine(stereo.rt, buffer, 1000.0, 48000)
                                                                : std::vector<float>{};
        const double l = out.empty() ? -1.0 : 20.0 * std::log10(amplitude_at(out.data() + 24000 * kChannels, 24000, kChannels, 1000.0, kRate));
        const double r = out.empty() ? -1.0 : 20.0 * std::log10(amplitude_at(out.data() + 24000 * kChannels + 1, 24000, kChannels, 1000.0, kRate));
        std::printf("  %-66s %+.3f / %+.3f dB\n", "levels on stereo, L / R", l, r);
        check(!out.empty() && std::abs(l) < 0.01 && std::abs(r) < 0.01 && polarity_of(out, kChannels, 1) == 1,
              "on stereo the values for speakers it lacks are dropped and play nowhere");
        if (SUCCEEDED(stereo_locked)) stereo.config->UnlockForProcess();
        stereo.release();
    }

    // ------------------------------------------------------------------
    std::printf("\nformats the header describes\n");
    {
        // Upstream EqualizerAPO::LockForProcess, render side: the output
        // format's mask, the input's when the output gives none.
        UNCOMPRESSEDAUDIOFORMAT back51 = format;
        back51.dwSamplesPerFrame = 6;
        back51.dwChannelMask = 0x3F;
        UNCOMPRESSEDAUDIOFORMAT side51 = back51;
        side51.dwChannelMask = 0x60F;
        UNCOMPRESSEDAUDIOFORMAT nomask = back51;
        nomask.dwChannelMask = 0;
        IAudioMediaType* back_media = nullptr;
        IAudioMediaType* side_media = nullptr;
        IAudioMediaType* nomask_media = nullptr;
        CreateAudioMediaTypeFromUncompressedAudioFormat(&back51, &back_media);
        CreateAudioMediaTypeFromUncompressedAudioFormat(&side51, &side_media);
        CreateAudioMediaTypeFromUncompressedAudioFormat(&nomask, &nomask_media);
        std::vector<float> six_buffer(size_t{kMaxFrames} * 6);
        const auto mask_after_lock = [&](IAudioMediaType* in_media, IAudioMediaType* out_media) {
            APO_CONNECTION_DESCRIPTOR in_d = descriptor;
            in_d.pBuffer = reinterpret_cast<UINT_PTR>(six_buffer.data());
            in_d.pFormat = in_media;
            APO_CONNECTION_DESCRIPTOR out_d = in_d;
            out_d.pFormat = out_media;
            APO_CONNECTION_DESCRIPTOR* ins[1] = {&in_d};
            APO_CONNECTION_DESCRIPTOR* outs[1] = {&out_d};
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
            const HRESULT locked = apo.config->LockForProcess(1, ins, 1, outs);
            const uint32_t mask = SUCCEEDED(locked) ? shared->hdr.speaker_mask : 0xFFFFFFFFu;
            if (SUCCEEDED(locked)) apo.config->UnlockForProcess();
            apo.release();
            return mask;
        };
        const uint32_t from_output = mask_after_lock(back_media, side_media);
        const uint32_t from_input = mask_after_lock(back_media, nomask_media);
        std::printf("  %-66s 0x%x / 0x%x\n", "mask for 0x3F in, 0x60F out / 0x3F in, 0 out", from_output, from_input);
        check(from_output == 0x60F && from_input == 0x3F,
              "alone, the speaker mask is the output's, or the input's when the output has none");
        if (back_media) back_media->Release();
        if (side_media) side_media->Release();
        if (nomask_media) nomask_media->Release();
    }
    {
        // A 48 kHz stereo instance owns the ring; a 16 kHz 5.1 instance on the
        // same endpoint locks and runs beside it. The header must keep saying
        // what the ring carries.
        UNCOMPRESSEDAUDIOFORMAT comms = format;
        comms.fFramesPerSecond = 16000.0f;
        comms.dwSamplesPerFrame = 6;
        comms.dwChannelMask = 0x60F;
        IAudioMediaType* comms_media = nullptr;
        CreateAudioMediaTypeFromUncompressedAudioFormat(&comms, &comms_media);
        std::vector<float> comms_buffer(size_t{kMaxFrames} * 6);
        APO_CONNECTION_DESCRIPTOR comms_d = descriptor;
        comms_d.pBuffer = reinterpret_cast<UINT_PTR>(comms_buffer.data());
        comms_d.pFormat = comms_media;
        APO_CONNECTION_DESCRIPTOR* comms_ds[1] = {&comms_d};

        const auto process_comms = [&](IAudioProcessingObjectRT* rt) {
            APO_CONNECTION_PROPERTY in{};
            in.pBuffer = reinterpret_cast<UINT_PTR>(comms_buffer.data());
            in.u32ValidFrameCount = kMaxFrames;
            in.u32BufferFlags = BUFFER_VALID;
            in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY o = in;
            APO_CONNECTION_PROPERTY* ins[1] = {&in};
            APO_CONNECTION_PROPERTY* outs[1] = {&o};
            rt->APOProcess(1, ins, 1, outs);
        };
        const auto header_is = [&](uint32_t rate, uint32_t channels, uint32_t mask) {
            return shared->hdr.sample_rate == rate && shared->hdr.channels == channels &&
                   shared->hdr.speaker_mask == mask && ring->channels == channels;
        };

        Apo media_apo, comms_apo;
        create_apo(factory, &media_apo);
        create_apo(factory, &comms_apo);
        media_apo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        comms_apo.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
        const HRESULT media_locked = media_apo.config->LockForProcess(1, descriptors, 1, descriptors);
        const HRESULT comms_locked = comms_apo.config->LockForProcess(1, comms_ds, 1, comms_ds);
        run_sine(media_apo.rt, buffer, 1000.0, kMaxFrames);
        process_comms(comms_apo.rt);
        check(SUCCEEDED(media_locked) && SUCCEEDED(comms_locked) && header_is(48000, 2, 0x3),
              "a second instance's lock does not overwrite the ring owner's format");
        media_apo.config->UnlockForProcess();
        process_comms(comms_apo.rt);
        check(header_is(16000, 6, 0x60F), "the instance that takes the ring over publishes its own format");
        comms_apo.config->UnlockForProcess();
        media_apo.release();
        comms_apo.release();
        if (comms_media) comms_media->Release();
    }

    // ------------------------------------------------------------------
    std::printf("\nreal-time path\n");
    {
        // A region no one has written yet, so every ring page starts untouched.
        // A full lap of the ring must not fault a page in on the audio thread:
        // LockForProcess touches them first.
        GUID fresh_endpoint{};
        CoCreateGuid(&fresh_endpoint);
        wchar_t fresh_text[64] = {};
        StringFromGUID2(fresh_endpoint, fresh_text, 64);
        FakeEndpointProperties fresh_properties(fresh_text);
        APOInitSystemEffects fresh_init = init;
        fresh_init.pAPOEndpointProperties = &fresh_properties;
        Apo apo;
        create_apo(factory, &apo);
        apo.apo->Initialize(sizeof(fresh_init), reinterpret_cast<BYTE*>(&fresh_init));
        const HRESULT locked = apo.config->LockForProcess(1, descriptors, 1, descriptors);
        for (size_t i = 0; i < sample_count; ++i) buffer[i] = 0.25f;
        APO_CONNECTION_PROPERTY in{};
        in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
        in.u32ValidFrameCount = kMaxFrames;
        in.u32BufferFlags = BUFFER_VALID;
        in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY o = in;
        APO_CONNECTION_PROPERTY* ins[1] = {&in};
        APO_CONNECTION_PROPERTY* outs[1] = {&o};
        const UINT32 lap_calls = isotone::kRingCapacityFrames / kMaxFrames + 2;
        PROCESS_MEMORY_COUNTERS before{}, after{};
        GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
        for (UINT32 call = 0; call < lap_calls; ++call) apo.rt->APOProcess(1, ins, 1, outs);
        GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
        const DWORD faults = after.PageFaultCount - before.PageFaultCount;
        std::printf("  %-66s %lu\n", "page faults over a full lap of the ring, stereo", static_cast<unsigned long>(faults));
        check(SUCCEEDED(locked) && faults < 8, "a full lap of the ring takes no page faults on the audio thread");
        apo.config->UnlockForProcess();
        apo.release();
    }

    // ------------------------------------------------------------------
    std::printf("\nchild APO (the one an install replaced)\n");
    {
        static TestChildFactory child_factory;
        DWORD cookie = 0;
        check_hr(CoRegisterClassObject(kTestChildClsid, &child_factory, CLSCTX_INPROC_SERVER,
                                       REGCLS_MULTIPLEUSE, &cookie),
                 "a test child APO class is registered in this process");

        // A fresh endpoint, so each case starts from a new region and its
        // -12 dB seed.
        GUID child_endpoint{};
        CoCreateGuid(&child_endpoint);
        wchar_t child_upper[64] = {};
        StringFromGUID2(child_endpoint, child_upper, 64);
        const std::wstring record_key = std::wstring(L"Software\\IsoAPO-selftest\\Child APOs\\") + child_upper;
        const auto record = [&](const std::wstring& value) {
            return RegSetKeyValueW(HKEY_CURRENT_USER, record_key.c_str(), L"PostMixChild", REG_SZ, value.c_str(),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        };
        const auto clsid_text = [](const CLSID& c) {
            wchar_t text[64] = {};
            StringFromGUID2(c, text, 64);
            return std::wstring(text);
        };

        FakeEndpointProperties child_properties(child_upper);
        APOInitSystemEffects child_init = init;
        child_init.pAPOEndpointProperties = &child_properties;

        struct Result {
            HRESULT init = E_FAIL, format = E_FAIL, lock = E_FAIL;
            HNSTIME latency = -1;
            double level = 0.0;
        };
        std::vector<float> separate(sample_count);
        const auto run = [&](bool separate_buffers = false) {
            Result r;
            Apo apo;
            if (!create_apo(factory, &apo)) return r;
            r.init = apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            apo.apo->GetLatency(&r.latency);
            IAudioMediaType* supported = nullptr;
            r.format = apo.apo->IsInputFormatSupported(media, media, &supported);
            if (supported) supported->Release();
            r.lock = apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const std::vector<float> out =
                run_sine(apo.rt, buffer, 1000.0, 48000, nullptr, separate_buffers ? separate.data() : nullptr);
            r.level = out.empty() ? 0.0 : settled_db(out, 1000.0);
            apo.config->UnlockForProcess();
            apo.release();
            return r;
        };
        const double alone = peaking_db(1000.0, -12.0, 1.0, 1000.0, kRate);
        const double with_child = alone + 20.0 * std::log10(kChildGain);

        check(record(clsid_text(kTestChildClsid)), "the child is recorded as the endpoint's post-mix child");
        g_child = ChildProbe{};
        Result r = run();
        std::printf("  %-66s %+.3f dB (child then band %+.3f)\n", "level at 1 kHz with the child", r.level, with_child);
        check(SUCCEEDED(r.init) && g_child.created == 1 && g_child.initialized == 1,
              "Initialize creates and initializes the child");
        check(g_child.endpoint == child_upper && g_child.init_clsid == post_mix,
              "the child gets the same initialization data");
        check(r.latency == kChildLatency, "GetLatency reports the child's latency");
        // The base class's LockForProcess asks IsInputFormatSupported again, so
        // the child sees that call as well as the explicit one.
        check(r.format == S_OK && g_child.format_calls == 2, "format negotiation goes to the child");
        check(SUCCEEDED(r.lock) && g_child.locks == 1 && g_child.unlocks == 1,
              "LockForProcess and UnlockForProcess are forwarded");
        check(g_child.processes == static_cast<int>((48000 + kMaxFrames - 1) / kMaxFrames),
              "the child runs on every process call");
        check(std::abs(r.level - with_child) < 0.01, "IsoAPO processes the child's output");
        check(g_child.live == 0, "releasing IsoAPO releases the child");

        g_child = ChildProbe{};
        r = run(true);
        check(std::abs(r.level - with_child) < 0.01,
              "with separate input and output buffers IsoAPO still processes the child's output");

        g_child = ChildProbe{};
        g_child.fail_initialize = true;
        r = run();
        check(SUCCEEDED(r.init) && r.latency == 0 && g_child.live == 0 && g_child.processes == 0 &&
                  std::abs(r.level - alone) < 0.01,
              "a child that fails Initialize is dropped and IsoAPO runs alone");

        const CLSID unregistered = {0x0badc0de, 0x1111, 0x2222, {0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44}};
        for (const auto& [value, what] : {
                 std::pair<std::wstring, const char*>{clsid_text(unregistered), "an unregistered child CLSID is skipped"},
                 {clsid_text(post_mix), "IsoAPO recorded as its own child is skipped"},
                 {L"!VALUE", "upstream's !VALUE placeholder means no child"},
                 {L"", "an empty record means no child"}}) {
            g_child = ChildProbe{};
            record(value);
            r = run();
            check(SUCCEEDED(r.init) && SUCCEEDED(r.lock) && r.latency == 0 && g_child.created == 0 &&
                      std::abs(r.level - alone) < 0.01,
                  what);
        }

        // Upstream EqualizerAPO::IsInputFormatSupported drops a child that
        // refuses a format and answers with the base class, so the endpoint
        // keeps its EQ.
        record(clsid_text(kTestChildClsid));
        g_child = ChildProbe{};
        g_child.refuse_formats = 1000;
        r = run();
        check(r.format == S_OK && SUCCEEDED(r.lock) && g_child.locks == 0 && g_child.processes == 0 &&
                  std::abs(r.level - alone) < 0.01,
              "a child that refuses every format is dropped and IsoAPO runs alone");
        // The base class's LockForProcess asks again after the child locked.
        g_child = ChildProbe{};
        g_child.refuse_locked = true;
        r = run();
        check(SUCCEEDED(r.lock) && g_child.locks == 1 && g_child.unlocks == 1 && g_child.processes == 0 &&
                  std::abs(r.level - alone) < 0.01,
              "a child that refuses the format it locked is unlocked, dropped, and IsoAPO runs alone");
        // While locked the audio thread may be running the child: a refused
        // probe then must not release it.
        g_child = ChildProbe{};
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            const HRESULT locked = apo.config->LockForProcess(1, descriptors, 1, descriptors);
            g_child.refuse_formats = 1;
            IAudioMediaType* supported = nullptr;
            const HRESULT refused = apo.apo->IsInputFormatSupported(media, media, &supported);
            if (supported) supported->Release();
            const std::vector<float> out = run_sine(apo.rt, buffer, 1000.0, 48000);
            const double level = out.empty() ? 0.0 : settled_db(out, 1000.0);
            apo.config->UnlockForProcess();
            apo.release();
            check(SUCCEEDED(locked) && FAILED(refused) && std::abs(level - with_child) < 0.01,
                  "a child that refuses a probe while IsoAPO is locked keeps running");
        }

        // A second unlock must not give the child an unbalanced unlock.
        g_child = ChildProbe{};
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            const HRESULT locked = apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const HRESULT unlocked = apo.config->UnlockForProcess();
            const HRESULT again = apo.config->UnlockForProcess();
            apo.release();
            check(SUCCEEDED(locked) && SUCCEEDED(unlocked) && again == APOERR_ALREADY_UNLOCKED &&
                      g_child.locks == 1 && g_child.unlocks == 1,
                  "UnlockForProcess while unlocked is refused before the child is touched");
        }

        // A silent buffer holds whatever was there before; the child must see
        // silence, or it filters leftovers into the output.
        g_child = ChildProbe{};
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            apo.config->LockForProcess(1, descriptors, 1, descriptors);
            bool quiet = true;
            for (int call = 0; call < 50; ++call) {
                for (size_t i = 0; i < sample_count; ++i) buffer[i] = 0.5f;
                APO_CONNECTION_PROPERTY in{};
                in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
                in.u32ValidFrameCount = kMaxFrames;
                in.u32BufferFlags = BUFFER_SILENT;
                in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
                APO_CONNECTION_PROPERTY out = in;
                APO_CONNECTION_PROPERTY* ins[1] = {&in};
                APO_CONNECTION_PROPERTY* outs[1] = {&out};
                apo.rt->APOProcess(1, ins, 1, outs);
                for (size_t i = 0; i < sample_count; ++i) quiet &= buffer[i] == 0.0f;
            }
            apo.config->UnlockForProcess();
            apo.release();
            check(quiet, "a child is given silence, not a silent buffer's leftovers");
        }

        // IsoAPO's own lock failing after the child locked must unlock the child.
        g_child = ChildProbe{};
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            UNCOMPRESSEDAUDIOFORMAT other_rate = format;
            other_rate.fFramesPerSecond = 44100.0f;   // the base class requires matching rates
            IAudioMediaType* other_media = nullptr;
            CreateAudioMediaTypeFromUncompressedAudioFormat(&other_rate, &other_media);
            APO_CONNECTION_DESCRIPTOR out_descriptor = descriptor;
            out_descriptor.pFormat = other_media;
            APO_CONNECTION_DESCRIPTOR* outs[1] = {&out_descriptor};
            const HRESULT locked = apo.config->LockForProcess(1, descriptors, 1, outs);
            apo.release();
            if (other_media) other_media->Release();
            check(FAILED(locked) && g_child.locks == 1 && g_child.unlocks == 1,
                  "a failed lock unlocks a child that had already locked");
        }

        // Equalizer APO recorded as the child is refused, so the two can never
        // host each other. The class is served by the test factory here, so a
        // missing guard shows up as a created child, not a real Equalizer APO.
        const CLSID eapo_post = {0xec1cc9ce, 0xfaed, 0x4822, {0x82, 0x8a, 0x82, 0xa8, 0x1a, 0x6f, 0x01, 0x8f}};
        DWORD eapo_cookie = 0;
        CoRegisterClassObject(eapo_post, &child_factory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &eapo_cookie);
        g_child = ChildProbe{};
        record(clsid_text(eapo_post));
        r = run();
        check(SUCCEEDED(r.init) && g_child.created == 0 && std::abs(r.level - alone) < 0.01,
              "Equalizer APO recorded as the child is refused");
        CoRevokeClassObject(eapo_cookie);

        record(clsid_text(kTestChildClsid));

        // A wrapper APO whose own record names IsoAPO: each would create the
        // other until the stack runs out. The IsoAPO created inside IsoAPO's
        // child refuses to initialize, so the wrapper runs alone inside IsoAPO.
        g_child = ChildProbe{};
        g_child.host_isoapo = true;
        r = run();
        check(SUCCEEDED(r.init) && g_child.created == 1 && FAILED(g_child.nested_init) &&
                  std::abs(r.level - with_child) < 0.01,
              "an IsoAPO created inside its own child refuses, so the chain ends");

        // A child that fails its lock on a format IsoAPO can process alone is
        // dropped; on a format only the child can convert, the lock fails and
        // the child is kept for the next attempt.
        g_child = ChildProbe{};
        g_child.fail_lock = true;
        r = run();
        check(SUCCEEDED(r.lock) && g_child.live == 0 && std::abs(r.level - alone) < 0.01,
              "a child that fails LockForProcess on the stream's own format is dropped");
        g_child = ChildProbe{};
        {
            UNCOMPRESSEDAUDIOFORMAT six = format;
            six.dwSamplesPerFrame = 6;
            six.dwChannelMask = 0x60F;
            IAudioMediaType* six_media = nullptr;
            CreateAudioMediaTypeFromUncompressedAudioFormat(&six, &six_media);
            std::vector<float> six_buffer(size_t{kMaxFrames} * 6);
            APO_CONNECTION_DESCRIPTOR six_out = descriptor;
            six_out.pFormat = six_media;
            six_out.pBuffer = reinterpret_cast<UINT_PTR>(six_buffer.data());
            APO_CONNECTION_DESCRIPTOR* six_outs[1] = {&six_out};
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            g_child.fail_lock = true;
            const HRESULT refused = apo.config->LockForProcess(1, descriptors, 1, six_outs);
            const bool kept = g_child.live == 1;
            g_child.fail_lock = false;
            const HRESULT retried = apo.config->LockForProcess(1, descriptors, 1, six_outs);
            if (SUCCEEDED(retried)) apo.config->UnlockForProcess();
            apo.release();
            check(FAILED(refused) && kept && SUCCEEDED(retried) && g_child.locks == 2,
                  "a child that fails LockForProcess on a channel change it negotiated fails the lock and is kept");

            // Locks the channel change, then refuses it when the base class asks:
            // dropped, and IsoAPO alone cannot change the channel count.
            g_child = ChildProbe{};
            g_child.refuse_locked = true;
            Apo changer;
            create_apo(factory, &changer);
            changer.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            const HRESULT changed = changer.config->LockForProcess(1, descriptors, 1, six_outs);
            const HRESULT unlocked = changer.config->UnlockForProcess();
            changer.release();
            if (six_media) six_media->Release();
            check(FAILED(changed) && unlocked == APOERR_ALREADY_UNLOCKED && g_child.locks == 1 && g_child.unlocks == 1,
                  "a child that refuses the channel change it locked fails the lock, leaving nothing locked");
        }

        // A second lock while locked must not touch the child the audio thread
        // is using, and Reset reaches the child.
        g_child = ChildProbe{};
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const HRESULT again = apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const std::vector<float> out = run_sine(apo.rt, buffer, 1000.0, 48000);
            apo.apo->Reset();
            check(again == APOERR_APO_LOCKED && g_child.locks == 1 && g_child.live == 1 && !out.empty() &&
                      std::abs(settled_db(out, 1000.0) - with_child) < 0.01,
                  "LockForProcess while locked is refused before the child is touched");
            check(g_child.resets == 1, "Reset is forwarded to the child");
            apo.config->UnlockForProcess();
            apo.release();
        }

        // A child that leaves the output flags alone: the flags IsoAPO wrote on
        // the previous call must not silence this one.
        g_child = ChildProbe{};
        g_child.skip_output_flags = true;
        {
            Apo apo;
            create_apo(factory, &apo);
            apo.apo->Initialize(sizeof(child_init), reinterpret_cast<BYTE*>(&child_init));
            apo.config->LockForProcess(1, descriptors, 1, descriptors);
            std::vector<float> out;
            APO_CONNECTION_PROPERTY in{}, o{};
            o.u32BufferFlags = BUFFER_VALID;
            for (UINT32 call = 0; call < 48; ++call) {
                const bool silent = call == 0;
                for (UINT32 i = 0; i < kMaxFrames; ++i) {
                    const double t = static_cast<double>(size_t{call} * kMaxFrames + i);
                    const float v = silent ? 0.0f : static_cast<float>(std::sin(2.0 * kPi * 1000.0 * t / kRate));
                    for (UINT32 c = 0; c < kChannels; ++c) buffer[i * kChannels + c] = v;
                }
                in.pBuffer = reinterpret_cast<UINT_PTR>(buffer);
                in.u32ValidFrameCount = kMaxFrames;
                in.u32BufferFlags = silent ? BUFFER_SILENT : BUFFER_VALID;
                in.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
                o.pBuffer = in.pBuffer;
                o.u32ValidFrameCount = kMaxFrames;
                o.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;   // flags carry over, as the engine's do
                APO_CONNECTION_PROPERTY* ins[1] = {&in};
                APO_CONNECTION_PROPERTY* outs[1] = {&o};
                apo.rt->APOProcess(1, ins, 1, outs);
                if (!silent) out.insert(out.end(), buffer, buffer + sample_count);
            }
            apo.config->UnlockForProcess();
            apo.release();
            check(std::abs(settled_db(out, 1000.0) - with_child) < 0.01,
                  "a child that does not set output flags is not silenced by the last call's");
        }

        // A discovery-only instance does no work: no region, no child.
        g_child = ChildProbe{};
        {
            APOInitSystemEffects2 discovery{};
            discovery.APOInit.cbSize = sizeof(discovery);
            discovery.APOInit.clsid = post_mix;
            discovery.pAPOEndpointProperties = &child_properties;
            discovery.InitializeForDiscoveryOnly = TRUE;
            Apo apo;
            create_apo(factory, &apo);
            const HRESULT initialized = apo.apo->Initialize(sizeof(discovery), reinterpret_cast<BYTE*>(&discovery));
            isotone::win::SharedMapping region;
            const DWORD opened = region.open(isotone::win::mapping_name(L"Local\\", child_upper));
            apo.release();
            check(SUCCEEDED(initialized) && opened == ERROR_FILE_NOT_FOUND && g_child.created == 0,
                  "an instance initialized for discovery only creates no region and no child");
        }

        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\IsoAPO-selftest");
        CoRevokeClassObject(cookie);
    }

    // ------------------------------------------------------------------
    std::printf("\npersisted state\n");
    {
        const std::wstring dir = isotone::win::persisted_state_dir(true);
        std::vector<std::wstring> written;
        const auto fresh = [] {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t text[64] = {};
            StringFromGUID2(g, text, 64);
            return std::wstring(text);
        };
        const auto persist = [&](const std::wstring& guid, const isotone::EqState& s) {
            isotone::ParamBlock b{};
            isotone::init_param_block(&b);
            isotone::to_param_block(s, &b);
            written.push_back(isotone::win::persisted_state_path(dir, guid));
            return isotone::win::write_persisted_state(written.back(), b) == ERROR_SUCCESS;
        };
        struct Instance {
            Apo apo;
            FakeEndpointProperties properties;
            APOInitSystemEffects init{};
            HRESULT initialized = E_FAIL;
        };
        const auto open_instance = [&](Instance* in) {
            create_apo(factory, &in->apo);
            in->init.APOInit.cbSize = sizeof(in->init);
            in->init.APOInit.clsid = post_mix;
            in->init.pAPOEndpointProperties = &in->properties;
            in->initialized = in->apo.apo->Initialize(sizeof(in->init), reinterpret_cast<BYTE*>(&in->init));
        };

        {
            // A block converted without init_param_block has no magic, version
            // or size; the file must still be one IsoAPO reads.
            const std::wstring guid = fresh();
            isotone::ParamBlock raw{};
            isotone::EqState s = peaking_state(1000.0, -6.0, 1.0);
            s.layout_channels = 8;
            s.layout_speaker_mask = 0x63F;
            isotone::to_param_block(s, &raw);
            raw.hdr.channels = 2;
            raw.hdr.speaker_mask = 0x3;
            written.push_back(isotone::win::persisted_state_path(dir, guid));
            const DWORD saved = isotone::win::write_persisted_state(written.back(), raw);
            isotone::ParamBlock back{};
            const isotone::win::PersistedRead read = isotone::win::read_persisted_state(written.back(), &back);
            check(saved == ERROR_SUCCESS && read == isotone::win::PersistedRead::Loaded && back.band_count == 1 &&
                      back.bands[0].gain_db == -6.0f,
                  "a block written without an initialised header is read back with its parameters");
            check(back.layout_channels == 8 && back.layout_speaker_mask == 0x63F && back.hdr.channels == 0 &&
                      back.hdr.speaker_mask == 0,
                  "the saved file keeps the layout the state was written for, not the host's format");
        }
        {
            const std::wstring guid = fresh();
            check(persist(guid, isotone::EqState{}), "a flat state is saved for a device");
            Instance a{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&a);
            isotone::win::SharedMapping region;
            const bool opened = region.open(isotone::win::mapping_name(L"Local\\", guid)) == ERROR_SUCCESS;
            check(SUCCEEDED(a.initialized) && opened && region.params()->band_count == 0,
                  "a saved flat state is the device's state, not the default");
            a.apo.release();
        }
        {
            const std::wstring guid = fresh();
            isotone::EqState muted;
            muted.mute = true;
            muted.channel_gain_db[1] = -3.0;
            muted.speakers.bass_management = true;
            persist(guid, muted);
            Instance a{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&a);
            isotone::win::SharedMapping region;
            const bool opened = region.open(isotone::win::mapping_name(L"Local\\", guid)) == ERROR_SUCCESS;
            check(opened && region.params()->mute == 1 && region.params()->channel_gain_db[1] == -3.0f &&
                      (region.params()->speakers.flags & isotone::kSpeakerFlagBassManagement) != 0 &&
                      region.params()->band_count == 0,
                  "mute, trims and the speaker setup start from the saved state");
            a.apo.release();
        }
        {
            const std::wstring guid = fresh();
            persist(guid, isotone::EqState{});
            const HANDLE f = CreateFileW(written.back().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD n = 0;
            WriteFile(f, "garbage", 7, &n, nullptr);
            CloseHandle(f);
            Instance a{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&a);
            isotone::win::SharedMapping region;
            const bool opened = region.open(isotone::win::mapping_name(L"Local\\", guid)) == ERROR_SUCCESS;
            check(opened && region.params()->band_count == 0, "a saved file that is not a valid block starts flat");
            a.apo.release();
        }
        {
            // The UI died mid-write: the region's seqlock stays odd. A new
            // instance locks with the saved state rather than nothing.
            const std::wstring guid = fresh();
            persist(guid, peaking_state(1000.0, -6.0, 1.0));
            Instance a{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&a);
            isotone::win::SharedMapping region;
            region.open(isotone::win::mapping_name(L"Local\\", guid));
            if (region.is_open()) {
                ui_write(region.params(), peaking_state(1000.0, -3.0, 1.0));
                region.params()->hdr.seq |= 1u;
            }
            Instance b{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&b);
            const HRESULT locked = b.apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const std::vector<float> out = SUCCEEDED(locked) ? run_sine(b.apo.rt, buffer, 1000.0, 48000) : std::vector<float>{};
            const double level = out.empty() ? 0.0 : settled_db(out, 1000.0);
            std::printf("  %-66s %+.3f dB\n", "level locked on a region stuck mid-write (saved -6 dB)", level);
            check(region.is_open() && SUCCEEDED(locked) && std::abs(level + 6.0) < 0.01,
                  "a lock with no consistent block plays the saved state");
            if (SUCCEEDED(locked)) b.apo.config->UnlockForProcess();
            b.apo.release();
            a.apo.release();
        }
        {
            // Something else holds the region's name at Initialize, so no region
            // can be made; once it is gone, LockForProcess makes it.
            const std::wstring guid = fresh();
            persist(guid, peaking_state(1000.0, -6.0, 1.0));
            const std::wstring region_name = isotone::win::mapping_name(L"Local\\", guid);
            HANDLE squatter = CreateEventW(nullptr, TRUE, FALSE, region_name.c_str());
            Instance a{Apo{}, FakeEndpointProperties(guid)};
            open_instance(&a);
            isotone::win::SharedMapping region;
            const DWORD before = region.open(region_name);
            CloseHandle(squatter);
            const HRESULT locked = a.apo.config->LockForProcess(1, descriptors, 1, descriptors);
            const bool opened = region.open(region_name) == ERROR_SUCCESS;
            check(squatter != nullptr && before != ERROR_SUCCESS && SUCCEEDED(a.initialized) && SUCCEEDED(locked) &&
                      opened && region.params()->band_count == 1 && region.params()->bands[0].gain_db == -6.0f &&
                      region.params()->hdr.host_state == static_cast<uint32_t>(isotone::HostState::Running),
                  "a region that could not be made at Initialize is made at LockForProcess");
            if (SUCCEEDED(locked)) a.apo.config->UnlockForProcess();
            a.apo.release();
        }

        for (const std::wstring& path : written) DeleteFileW(path.c_str());
    }

    _aligned_free(buffer);
    media->Release();
    factory->Release();

    check(can_unload() == S_OK, "DllCanUnloadNow says yes again after release");

    // A region whose magic someone zeroed must not stall the next open.
    {
        GUID stalled{};
        CoCreateGuid(&stalled);
        wchar_t stalled_text[64] = {};
        StringFromGUID2(stalled, stalled_text, 64);
        const std::wstring stalled_name = isotone::win::mapping_name(L"Local\\", stalled_text);
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(isotone::kSharedRegionBytes), stalled_name.c_str());
        isotone::win::SharedMapping opener;
        const auto t0 = std::chrono::steady_clock::now();
        const DWORD opened = opener.open(stalled_name);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %-66s %lld ms\n", "opening a region with a zero magic", static_cast<long long>(ms));
        check(h != nullptr && opened == ERROR_INVALID_DATA && ms < 150,
              "a region with a zeroed magic is refused within 150 ms");
        if (h) CloseHandle(h);
    }

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
