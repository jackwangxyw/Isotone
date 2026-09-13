// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

// DEFINE_GUID only declares the GUIDs unless INITGUID is defined first; this is
// the one translation unit that allocates them.
#include <initguid.h>

#include "isoapo.h"

#include <knownfolders.h>
#include <mmdeviceapi.h>
#include <shlobj.h>

#include <cmath>
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

// Where devicetool (upstream's install) records the replaced APO:
// <root>\<key>\{endpoint-guid}, values PreMixChild and PostMixChild. The
// self-test build reads a per-user copy it can write without elevation.
#ifdef ISOAPO_SELFTEST_LOCAL_NAMESPACE
HKEY child_root() { return HKEY_CURRENT_USER; }
constexpr wchar_t kChildApoKey[] = L"Software\\IsoAPO-selftest\\Child APOs";
#else
HKEY child_root() { return HKEY_LOCAL_MACHINE; }
constexpr wchar_t kChildApoKey[] = L"SOFTWARE\\IsoAPO\\Child APOs";
#endif

// The recorded child CLSID. Upstream's placeholders (!VALUE, !KEY) and an empty
// string are not CLSIDs and count as no child.
bool recorded_child(const std::wstring& endpoint_guid, bool pre_mix, CLSID* clsid) {
    const std::wstring key = std::wstring(kChildApoKey) + L"\\" + endpoint_guid;
    wchar_t value[64] = {};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(child_root(), key.c_str(), pre_mix ? L"PreMixChild" : L"PostMixChild",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    return value[0] == L'{' && SUCCEEDED(CLSIDFromString(value, clsid));
}

// Equalizer APO's own classes. Hosting one would put two EQs on the endpoint,
// and if Equalizer APO's record names IsoAPO in turn, each would create the
// other until the stack runs out.
const CLSID kEqualizerApoPreMix  = {0xeacd2258, 0xfcac, 0x4ff4, {0xb3, 0x6d, 0x41, 0x9e, 0x92, 0x4a, 0x6d, 0x79}};
const CLSID kEqualizerApoPostMix = {0xec1cc9ce, 0xfaed, 0x4822, {0x82, 0x8a, 0x82, 0xa8, 0x1a, 0x6f, 0x01, 0x8f}};

void debug_line(const wchar_t* text, HRESULT hr) {
    wchar_t message[256];
    swprintf_s(message, L"IsoAPO: %ls (0x%08lx)\n", text, static_cast<unsigned long>(hr));
    OutputDebugStringW(message);
}

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
    reset_child();
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
    // buffering. Phase shift is not latency. A child's latency is the chain's.
    *time = 0;
    if (child_ != nullptr) {
        child_->GetLatency(time);
    }
    return S_OK;
}

HRESULT IsoApo::Initialize(UINT32 size, BYTE* data) {
    if (locked_) {
        // Re-initializing would drop the child while the audio thread may be using it.
        return APOERR_ALREADY_INITIALIZED;
    }
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

    reset_child();
    try {
        const std::wstring guid = var.pwszVal;
        PropVariantClear(&var);
        open_shared_region(guid);
        create_child(guid, init->APOInit.clsid, size, data);
    } catch (const std::bad_alloc&) {
        PropVariantClear(&var);
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

// Upstream EqualizerAPO::Initialize: every failure drops the child and leaves
// IsoAPO running alone, so a broken vendor APO cannot take the endpoint down.
void IsoApo::create_child(const std::wstring& endpoint_guid, const CLSID& own_clsid, UINT32 size,
                          BYTE* data) {
    CLSID clsid{};
    if (!recorded_child(endpoint_guid, own_clsid == ISOAPO_PRE_MIX_GUID, &clsid)) {
        return;
    }
    // Hosting ourselves would recurse until the stack runs out.
    if (clsid == ISOAPO_POST_MIX_GUID || clsid == ISOAPO_PRE_MIX_GUID || clsid == kEqualizerApoPreMix ||
        clsid == kEqualizerApoPostMix) {
        return;
    }
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IAudioProcessingObject),
                                  reinterpret_cast<void**>(&child_));
    if (SUCCEEDED(hr)) {
        hr = child_->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&child_rt_));
    }
    if (SUCCEEDED(hr)) {
        hr = child_->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                    reinterpret_cast<void**>(&child_cfg_));
    }
    if (SUCCEEDED(hr)) {
        hr = child_->Initialize(size, data);
    }
    if (FAILED(hr)) {
        debug_line(L"child APO could not be created or initialized; running without it", hr);
        reset_child();
    }
}

void IsoApo::reset_child() {
    if (child_cfg_ != nullptr) child_cfg_->Release();
    if (child_rt_ != nullptr) child_rt_->Release();
    if (child_ != nullptr) child_->Release();
    child_cfg_ = nullptr;
    child_rt_ = nullptr;
    child_ = nullptr;
}

void IsoApo::open_shared_region(const std::wstring& endpoint_guid) {
    ring_.release();
    const std::wstring name = isotone::win::mapping_name(kObjectNamespace, endpoint_guid);
    const DWORD error = mapping_.create_or_open(name, &IsoApo::seed_region, this);
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

    // A region this instance created was seeded by seed_region before it became
    // visible. An existing one already holds what the UI or a sibling instance
    // put there, and that wins.
    ring_.attach(mapping_.ring(), isotone::kRingCapacityFrames);
}

void IsoApo::seed_region(isotone::ParamBlock* block, void* self) {
    auto* apo = static_cast<IsoApo*>(self);
    apo->load_parameters();
    isotone::param_block_write(block, [&](isotone::ParamBlock* b) { isotone::to_param_block(apo->state_, b); });
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

    // With a child, the child decides: it may convert the format, and IsoAPO
    // processes whatever it outputs. A refusal is an answer to this one probe,
    // not a failure: the engine goes on to try other formats, so the child stays.
    if (child_ != nullptr) {
        return child_->IsInputFormatSupported(output, requested, supported);
    }

    IAudioMediaType* base_supported = nullptr;
    HRESULT hr = CBaseAudioProcessingObject::IsInputFormatSupported(output, requested, &base_supported);
    if (hr != S_OK || output == nullptr) {
        if (supported != nullptr) *supported = base_supported;
        else if (base_supported != nullptr) base_supported->Release();
        return hr;
    }
    UNCOMPRESSEDAUDIOFORMAT in{};
    UNCOMPRESSEDAUDIOFORMAT out{};
    if (FAILED(requested->GetUncompressedAudioFormat(&in)) || FAILED(output->GetUncompressedAudioFormat(&out))) {
        if (supported != nullptr) *supported = base_supported;
        else if (base_supported != nullptr) base_supported->Release();
        return hr;
    }
    // Alone, IsoAPO filters in place and cannot change the channel count, so it
    // asks for input in the output's layout and lets the engine convert.
    if (in.dwSamplesPerFrame != out.dwSamplesPerFrame) {
        if (base_supported != nullptr) base_supported->Release();
        if (supported == nullptr) return E_POINTER;
        UNCOMPRESSEDAUDIOFORMAT suggestion = in;
        suggestion.dwSamplesPerFrame = out.dwSamplesPerFrame;
        suggestion.dwChannelMask = out.dwChannelMask;
        const HRESULT created = CreateAudioMediaTypeFromUncompressedAudioFormat(&suggestion, supported);
        return FAILED(created) ? created : S_FALSE;
    }
    if (supported != nullptr) *supported = base_supported;
    else if (base_supported != nullptr) base_supported->Release();
    return S_OK;
}

HRESULT IsoApo::LockForProcess(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs,
                               UINT32 outputCount, APO_CONNECTION_DESCRIPTOR** outputs) {
    if (inputs == nullptr || outputs == nullptr || inputCount == 0 || outputCount == 0) {
        return E_INVALIDARG;
    }
    // Nothing may throw into audiodg (plan 5.3). Allocation here is the delay
    // lines and band storage, and it can fail under memory pressure.
    try {
        return lock(inputCount, inputs, outputCount, outputs);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT IsoApo::lock(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
                     APO_CONNECTION_DESCRIPTOR** outputs) {
    UNCOMPRESSEDAUDIOFORMAT format{};
    HRESULT hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
    if (FAILED(hr)) {
        return hr;
    }
    UNCOMPRESSEDAUDIOFORMAT out_format{};
    hr = outputs[0]->pFormat->GetUncompressedAudioFormat(&out_format);
    if (FAILED(hr)) {
        return hr;
    }

    bool child_locked = false;
    if (child_cfg_ != nullptr) {
        hr = child_cfg_->LockForProcess(inputCount, inputs, outputCount, outputs);
        if (FAILED(hr)) {
            debug_line(L"child APO failed LockForProcess; running without it", hr);
            reset_child();
        } else {
            child_locked = true;
        }
    }
    // IsoAPO processes the child's output, so with a child the output format is
    // the one to process. Alone it filters in place and cannot change the
    // channel count.
    if (child_ != nullptr) {
        format = out_format;
    } else if (format.dwSamplesPerFrame != out_format.dwSamplesPerFrame) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    hr = CBaseAudioProcessingObject::LockForProcess(inputCount, inputs, outputCount, outputs);
    if (FAILED(hr)) {
        if (child_locked) {
            child_cfg_->UnlockForProcess();
        }
        return hr;
    }

    // From here on a failure must leave nothing locked.
    struct Undo {
        IsoApo* apo;
        bool child;
        bool armed = true;
        ~Undo() {
            if (!armed) return;
            if (child && apo->child_cfg_ != nullptr) apo->child_cfg_->UnlockForProcess();
            apo->CBaseAudioProcessingObject::UnlockForProcess();
        }
    } undo{this, child_locked};

    // Windows tears down and recreates an APO whenever the device format
    // changes, so every LockForProcess is a cold start (plan 5.4).
    channels_ = format.dwSamplesPerFrame;
    UNCOMPRESSEDAUDIOFORMAT in_format{};
    input_channels_ = SUCCEEDED(inputs[0]->pFormat->GetUncompressedAudioFormat(&in_format))
                          ? in_format.dwSamplesPerFrame
                          : channels_;
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

    undo.armed = false;
    locked_ = true;
    return S_OK;
}

HRESULT IsoApo::UnlockForProcess() {
    locked_ = false;
    ring_.release();
    if (child_cfg_ != nullptr) {
        const HRESULT hr = child_cfg_->UnlockForProcess();
        if (FAILED(hr)) debug_line(L"child APO failed UnlockForProcess", hr);
    }
    return CBaseAudioProcessingObject::UnlockForProcess();
}

void IsoApo::APOProcess(UINT32 inputCount, APO_CONNECTION_PROPERTY** inputs,
                        UINT32 outputCount, APO_CONNECTION_PROPERTY** outputs) {
    if (!locked_ || inputs == nullptr || outputs == nullptr) {
        return;
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

    // A silent buffer's contents are undefined; make them the silence they
    // stand for before anything reads them, as upstream does.
    const size_t in_samples = static_cast<size_t>(frames) *
                              (child_rt_ != nullptr ? input_channels_ : channels_);
    if (flags == BUFFER_SILENT) {
        memset(in, 0, in_samples * sizeof(float));
    }

    // The child runs first, on silent buffers too so its own state stays
    // continuous, and writes the output buffer IsoAPO then processes.
    bool silent = flags == BUFFER_SILENT;
    if (child_rt_ != nullptr) {
        child_rt_->APOProcess(inputCount, inputs, outputCount, outputs);
        if (outputs[0]->u32BufferFlags == BUFFER_SILENT) {
            memset(out, 0, static_cast<size_t>(frames) * channels_ * sizeof(float));
            silent = true;
        }
    } else if (in != out) {
        memcpy(out, in, static_cast<size_t>(frames) * channels_ * sizeof(float));
    }

    // MXCSR is per thread, and a child may have changed it, so flush-to-zero is
    // set again on every call, after the child.
    isotone::enable_denormal_flushing();

    // Silence goes through the processor too: its delay lines and filters keep
    // moving, so a delayed tail plays out and nothing stale waits in them for
    // the next sound.
    processor_.process_interleaved(out, frames);
    ring_.write(out, channels_, frames);

    bool audible = !silent;
    if (silent) {
        const size_t samples = static_cast<size_t>(frames) * channels_;
        for (size_t i = 0; i < samples && !audible; ++i) {
            audible = std::fabs(out[i]) > 1e-10f;
        }
    }
    outputs[0]->u32ValidFrameCount = frames;
    // BUFFER_SILENT matters to some drivers, so it is kept unless there really
    // is audio, such as a delay's tail.
    outputs[0]->u32BufferFlags = audible ? BUFFER_VALID : BUFFER_SILENT;
}
