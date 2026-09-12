// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

// DEFINE_GUID only declares the GUIDs unless INITGUID is defined first; this is
// the one translation unit that allocates them.
#include <initguid.h>

#include "isoapo.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <fstream>
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
}

IsoApo::~IsoApo() { InterlockedDecrement(&instanceCount); }

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

HRESULT IsoApo::Initialize(UINT32 /*size*/, BYTE* /*data*/) {
    // Nothing to do from the init payload yet. Stage 3 will use it to work out
    // which endpoint this instance belongs to, so it can open the right
    // shared-memory block.
    return S_OK;
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
    load_parameters();
    processor_.initialize(format.fFramesPerSecond, channels_, inputs[0]->u32MaxFrameCount,
                          isotone::kParamMaxBands);
    processor_.set_target(state_);
    processor_.reset();

    locked_ = true;
    return S_OK;
}

HRESULT IsoApo::UnlockForProcess() {
    locked_ = false;
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
        return;
    }

    if (in != out) {
        memcpy(out, in, static_cast<size_t>(frames) * channels_ * sizeof(float));
    }
    processor_.process_interleaved(out, frames);

    outputs[0]->u32ValidFrameCount = frames;
    outputs[0]->u32BufferFlags = BUFFER_VALID;
}
