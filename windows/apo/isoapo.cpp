// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

// DEFINE_GUID only declares the GUIDs unless INITGUID is defined first; this is
// the one translation unit that allocates them.
#include <initguid.h>

#include "isoapo.h"

#include <knownfolders.h>
#include <mmdeviceapi.h>
#include <shlobj.h>

#include <fstream>
#include <new>
#include <sstream>
#include <string>

#include "isotone/apo_config.h"
#include "isotone/param_block.h"

long IsoApo::instanceCount = 0;

// APO_FLAG_INPLACE lets the audio engine hand us the same buffer for input and
// output. The MUST_MATCH flags say we cannot resample or change bit depth, which
// is true: the core is a filter, not a format converter.
const CRegAPOProperties<1> IsoApo::regPostMixProperties(
    ISOAPO_POST_MIX_GUID, L"IsoAPO", L"Copyright (C) 2026", 1, 0,
    __uuidof(IAudioProcessingObject),
    (APO_FLAG)(APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
               APO_FLAG_INPLACE));

const CRegAPOProperties<1> IsoApo::regPreMixProperties(
    ISOAPO_PRE_MIX_GUID, L"IsoAPO", L"Copyright (C) 2026", 1, 0,
    __uuidof(IAudioProcessingObject),
    (APO_FLAG)(APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
               APO_FLAG_INPLACE));

namespace {

// The shipping DLL runs in audiodg (session 0) and the UI in the user's session,
// so the region must live in the Global\ namespace. Creating a Global\ object
// needs SeCreateGlobalPrivilege, which an unelevated test process lacks, so the
// self-test build of the same sources uses Local\ instead.
#ifdef ISOAPO_SELFTEST_LOCAL_NAMESPACE
constexpr wchar_t kObjectNamespace[] = L"Local\\";
#else
constexpr wchar_t kObjectNamespace[] = L"Global\\";
#endif

std::atomic<uint32_t> g_instance_serial{0};

// audiodg runs as LocalService and cannot read user directories, so the only
// place parameters can live is ProgramData (plan 5.4).
std::wstring config_path() {
    wchar_t* base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) {
        path = base;
        CoTaskMemFree(base);
        path += L"\\IsoAPO\\config.txt";
    }
    return path;
}

}  // namespace

IsoApo::IsoApo(IUnknown* outer) : CBaseAudioProcessingObject(regPostMixProperties) {
    outer_ = outer != nullptr
                 ? outer
                 : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    InterlockedIncrement(&instanceCount);
    ring_token_ = (uint64_t{GetCurrentProcessId()} << 32) | (g_instance_serial.fetch_add(1) + 1);
}

IsoApo::~IsoApo() {
    ring_.release();
    InterlockedDecrement(&instanceCount);
}

HRESULT IsoApo::QueryInterface(const IID& iid, void** ppv) {
    return outer_->QueryInterface(iid, ppv);
}

ULONG IsoApo::AddRef() { return outer_->AddRef(); }
ULONG IsoApo::Release() { return outer_->Release(); }

HRESULT IsoApo::NonDelegatingQueryInterface(const IID& iid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    if (iid == __uuidof(IUnknown)) {
        *ppv = static_cast<INonDelegatingUnknown*>(this);
    } else if (iid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (iid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    reinterpret_cast<IUnknown*>(*ppv)->AddRef();
    return S_OK;
}

ULONG IsoApo::NonDelegatingAddRef() { return InterlockedIncrement(&refCount_); }

ULONG IsoApo::NonDelegatingRelease() {
    const long count = InterlockedDecrement(&refCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count);
}

HRESULT IsoApo::GetLatency(HNSTIME* time) {
    if (time == nullptr) {
        return E_POINTER;
    }
    // A cascade of biquads is a zero-latency structure: no lookahead, no
    // buffering. Phase shift is not latency.
    *time = 0;
    return S_OK;
}

HRESULT IsoApo::Initialize(UINT32 size, BYTE* data) {
    // Same acceptance as upstream EqualizerAPO::Initialize, which reads the
    // endpoint GUID the same way. APOInitSystemEffects2 begins with the same
    // fields, so it is read through the smaller struct.
    if (size != sizeof(APOInitSystemEffects) && size != sizeof(APOInitSystemEffects2)) {
        return E_INVALIDARG;
    }
    if (data == nullptr) {
        return E_POINTER;
    }
    const auto* init = reinterpret_cast<APOInitSystemEffects*>(data);
    if (init->pAPOEndpointProperties == nullptr) {
        return E_POINTER;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    HRESULT hr = init->pAPOEndpointProperties->GetValue(PKEY_AudioEndpoint_GUID, &var);
    if (FAILED(hr)) {
        return hr;
    }
    if (var.vt != VT_LPWSTR || var.pwszVal == nullptr) {
        PropVariantClear(&var);
        return E_UNEXPECTED;
    }

    try {
        const std::wstring guid = var.pwszVal;
        PropVariantClear(&var);
        open_shared_region(guid);
    } catch (const std::bad_alloc&) {
        PropVariantClear(&var);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void IsoApo::open_shared_region(const std::wstring& endpoint_guid) {
    ring_.release();
    const std::wstring name = isotone::win::mapping_name(kObjectNamespace, endpoint_guid);
    const DWORD error = mapping_.create_or_open(name);
    if (error != ERROR_SUCCESS) {
        // Audio keeps flowing on the file-seeded parameters; failing Initialize
        // would take the endpoint's effects down with it. The UI sees no region
        // and reports the engine idle, and this line is visible in DebugView.
        wchar_t message[512];
        swprintf_s(message, L"IsoAPO: cannot create or open %ls, error %lu\n", name.c_str(),
                   error);
        OutputDebugStringW(message);
        return;
    }

    // Seed only a region this instance created. An existing one already holds
    // what the UI or a sibling instance put there, and that wins.
    if (mapping_.created()) {
        load_parameters();
        isotone::param_block_write(mapping_.params(), [&](isotone::ParamBlock* b) {
            isotone::to_param_block(state_, b);
        });
    }
    ring_.attach(mapping_.ring(), isotone::kRingCapacityFrames);
}

void IsoApo::load_parameters() {
    state_ = isotone::EqState{};

    const std::wstring path = config_path();
    if (!path.empty()) {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (in.good()) {
            std::ostringstream text;
            text << in.rdbuf();
            const isotone::ApoParseResult parsed = isotone::parse_apo_config(text.str());
            if (!parsed.state.bands.empty() || parsed.state.preamp_db != 0.0) {
                state_ = parsed.state;
                return;
            }
        }
    }

    // No config file: the stage 1 default, a -12 dB dip at 1 kHz, which is what
    // the acceptance measurement looks for.
    isotone::Band band;
    band.id = 1;
    band.type = isotone::FilterType::Peaking;
    band.fc = 1000.0;
    band.gain_db = -12.0;
    band.width = 1.0;
    band.width_mode = isotone::WidthMode::Q;
    state_.bands.push_back(band);
}

HRESULT IsoApo::IsInputFormatSupported(IAudioMediaType* output, IAudioMediaType* requested,
                                       IAudioMediaType** supported) {
    if (requested == nullptr) {
        return E_POINTER;
    }

    UNCOMPRESSEDAUDIOFORMAT in{};
    HRESULT hr = requested->GetUncompressedAudioFormat(&in);
    if (FAILED(hr)) {
        return hr;
    }
    UNCOMPRESSEDAUDIOFORMAT out{};
    hr = output->GetUncompressedAudioFormat(&out);
    if (FAILED(hr)) {
        return hr;
    }

    hr = CBaseAudioProcessingObject::IsInputFormatSupported(output, requested, supported);

    // Downmixing is out of scope, same as upstream: refuse a format with more
    // input channels than output channels.
    if (hr == S_OK && in.dwSamplesPerFrame > 2 && in.dwSamplesPerFrame > out.dwSamplesPerFrame) {
        CreateAudioMediaTypeFromUncompressedAudioFormat(&out, supported);
        hr = S_FALSE;
    }
    return hr;
}

HRESULT IsoApo::LockForProcess(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs,
                               UINT32 outputCount, APO_CONNECTION_DESCRIPTOR** outputs) {
    if (inputs == nullptr || outputs == nullptr || inputCount == 0 || outputCount == 0) {
        return E_INVALIDARG;
    }

    UNCOMPRESSEDAUDIOFORMAT format{};
    HRESULT hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
    if (FAILED(hr)) {
        return hr;
    }

    hr = CBaseAudioProcessingObject::LockForProcess(inputCount, inputs, outputCount, outputs);
    if (FAILED(hr)) {
        return hr;
    }

    // Windows tears down and recreates an APO whenever the device format
    // changes, so every LockForProcess is a cold start (plan 5.4).
    channels_ = format.dwSamplesPerFrame;
    // A stream that reports no mask gets upstream's default for its channel
    // count, which is also what the processor assumes for 0.
    const uint32_t speaker_mask =
        format.dwChannelMask != 0 ? format.dwChannelMask : isotone::default_speaker_mask(channels_);
    if (!mapping_.is_open()) {
        load_parameters();
    } else if (isotone::param_block_read(mapping_.params(), &block_, 1000)) {
        isotone::from_param_block(block_, &state_);
        applied_seq_ = block_.hdr.seq;
    } else {
        // No consistent read yet; keep the current state and make the first
        // process call try again.
        applied_seq_ = ~isotone::param_block_seq(mapping_.params());
    }
    // from_param_block reuses this capacity, so applying a block on the audio
    // thread never allocates.
    state_.bands.reserve(isotone::kParamMaxBands);

    processor_.initialize(format.fFramesPerSecond, channels_, inputs[0]->u32MaxFrameCount,
                          isotone::kParamMaxBands, speaker_mask);
    processor_.set_target(state_);
    processor_.reset();

    if (mapping_.is_open()) {
        isotone::host_publish_format(mapping_.params(), static_cast<uint32_t>(format.fFramesPerSecond),
                                     channels_, speaker_mask, isotone::HostState::Running);
        ring_.set_channels(channels_);
        ring_.claim(ring_token_);
    }

    locked_ = true;
    return S_OK;
}

HRESULT IsoApo::UnlockForProcess() {
    locked_ = false;
    ring_.release();
    return CBaseAudioProcessingObject::UnlockForProcess();
}

void IsoApo::APOProcess(UINT32 /*inputCount*/, APO_CONNECTION_PROPERTY** inputs,
                        UINT32 /*outputCount*/, APO_CONNECTION_PROPERTY** outputs) {
    if (!locked_ || inputs == nullptr || outputs == nullptr) {
        return;
    }

    // MXCSR is per thread, and this is the only place we are guaranteed to be on
    // audiodg's real-time thread.
    if (!denormals_set_) {
        isotone::enable_denormal_flushing();
        denormals_set_ = true;
    }

    if (mapping_.is_open()) {
        isotone::ParamBlock* shared = mapping_.params();
        isotone::host_heartbeat(shared);
        // Copy only when the UI has written since the last apply. A failed read
        // keeps the current parameters and retries on the next call.
        if (isotone::param_block_seq(shared) != applied_seq_ &&
            isotone::param_block_read(shared, &block_)) {
            applied_seq_ = block_.hdr.seq;
            isotone::from_param_block(block_, &state_);
            processor_.set_target(state_);
        }
        // Another instance may have released the ring since this one locked.
        if (!ring_.owns()) {
            ring_.claim(ring_token_);
        }
    }

    const UINT32 flags = inputs[0]->u32BufferFlags;
    if (flags != BUFFER_VALID && flags != BUFFER_SILENT) {
        return;
    }

    float* in  = reinterpret_cast<float*>(inputs[0]->pBuffer);
    float* out = reinterpret_cast<float*>(outputs[0]->pBuffer);
    const UINT32 frames = inputs[0]->u32ValidFrameCount;

    if (flags == BUFFER_SILENT) {
        // Silence in, silence out. Running the filters over zeros would only
        // burn cycles and let the tail ring on.
        memset(out, 0, static_cast<size_t>(frames) * channels_ * sizeof(float));
        outputs[0]->u32ValidFrameCount = frames;
        outputs[0]->u32BufferFlags = BUFFER_SILENT;
        ring_.write(nullptr, channels_, frames);
        return;
    }

    if (in != out) {
        memcpy(out, in, static_cast<size_t>(frames) * channels_ * sizeof(float));
    }
    processor_.process_interleaved(out, frames);
    ring_.write(out, channels_, frames);

    outputs[0]->u32ValidFrameCount = frames;
    outputs[0]->u32BufferFlags = BUFFER_VALID;
}
