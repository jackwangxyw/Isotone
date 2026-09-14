// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

// DEFINE_GUID only declares the GUIDs unless INITGUID is defined first; this is
// the one translation unit that allocates them.
#include <initguid.h>

#include "isoapo.h"

#include <mmdeviceapi.h>

#include <cmath>
#include <new>
#include <string>

#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "persisted_state.h"

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

// The high half of this process's ring tokens (audio_ring.h). Not the process
// id: Windows reuses ids, and a claim left by a crashed audiodg whose id the new
// audiodg got would read as held by an instance in this process and never be
// taken over. The performance counter when the first instance is made, mixed
// with the id, differs between the two. QueryPerformanceCounter cannot fail on
// Windows XP or later.
std::atomic<uint32_t> g_process_nonce{0};

uint32_t process_nonce() {
    uint32_t nonce = g_process_nonce.load();
    if (nonce != 0) return nonce;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    uint64_t x = static_cast<uint64_t>(counter.QuadPart) + uint64_t{GetCurrentProcessId()} * 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;   // splitmix64's finaliser
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const uint32_t candidate = static_cast<uint32_t>(x >> 32) | 1u;   // never 0, which means unset
    // A sibling made at the same moment may have set it first; theirs stands.
    g_process_nonce.compare_exchange_strong(nonce, candidate);
    return g_process_nonce.load();
}

// Nonzero while this thread is inside create_child. An IsoAPO initialized then
// is being created by its own child, directly or through other APOs, and each
// would create the other until the stack runs out. A plain int: no dynamic
// initialisation, so it needs no thread attach notifications.
thread_local int t_creating_child = 0;

#ifdef ISOAPO_SELFTEST_LOCAL_NAMESPACE
constexpr bool kSelftest = true;
#else
constexpr bool kSelftest = false;
#endif

}  // namespace

IsoApo::IsoApo(IUnknown* outer) : CBaseAudioProcessingObject(regPostMixProperties) {
    outer_ = outer != nullptr
                 ? outer
                 : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    InterlockedIncrement(&instanceCount);
    ring_token_ = (uint64_t{process_nonce()} << 32) | (g_instance_serial.fetch_add(1) + 1);
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

// Called with the stream stopped. Filter and delay state is cleared, so a
// restart does not play what was in the delay lines when it stopped.
HRESULT IsoApo::Reset() {
    if (child_ != nullptr) {
        child_->Reset();
    }
    processor_.reset();
    return S_OK;
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
    // Upstream EqualizerAPO::Initialize accepts only APOInitSystemEffects, and
    // reads the endpoint GUID the same way. APOInitSystemEffects2 is accepted
    // too, read through the smaller struct it begins with; audiodg passes it only
    // to an APO that implements IAudioSystemEffects2, which IsoAPO does not, so
    // only the self test sends it.
    if (size != sizeof(APOInitSystemEffects) && size != sizeof(APOInitSystemEffects2)) {
        return E_INVALIDARG;
    }
    if (data == nullptr) {
        return E_POINTER;
    }
    if (t_creating_child > 0) {
        debug_line(L"created inside its own child APO; refusing so the chain ends", E_FAIL);
        return E_FAIL;
    }
    const auto* init = reinterpret_cast<APOInitSystemEffects*>(data);
    if (init->pAPOEndpointProperties == nullptr) {
        return E_POINTER;
    }
    // An instance made only to read its properties is never locked: it needs no
    // region and no child.
    const bool discovery_only = size == sizeof(APOInitSystemEffects2) &&
                                reinterpret_cast<APOInitSystemEffects2*>(data)->InitializeForDiscoveryOnly;

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
        endpoint_guid_ = var.pwszVal;
        PropVariantClear(&var);
        if (!discovery_only) {
            open_shared_region();
            create_child(endpoint_guid_, init->APOInit.clsid, size, data);
        }
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
    // Hosting ourselves would recurse until the stack runs out. A cycle through
    // another APO is caught by t_creating_child instead.
    if (clsid == ISOAPO_POST_MIX_GUID || clsid == ISOAPO_PRE_MIX_GUID || clsid == kEqualizerApoPreMix ||
        clsid == kEqualizerApoPostMix) {
        return;
    }
    struct Depth {
        Depth() { ++t_creating_child; }
        ~Depth() { --t_creating_child; }
    } depth;
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

void IsoApo::open_shared_region() {
    ring_.release();
    // Read before the region exists: a region this instance creates stays
    // unfinished, and other instances wait on it, until it is seeded.
    state_ = saved_state();
    const std::wstring name = isotone::win::mapping_name(kObjectNamespace, endpoint_guid_);
    const DWORD error = mapping_.create_or_open(name, &IsoApo::seed_region, this);
    if (error != ERROR_SUCCESS) {
        // Audio keeps flowing on the saved parameters, and LockForProcess tries
        // again; failing Initialize would take the endpoint's effects down with
        // it. The UI sees no region and reports the engine idle, and this line
        // is visible in DebugView.
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
    isotone::param_block_write(block, [&](isotone::ParamBlock* b) { isotone::to_param_block(apo->state_, b); });
}

isotone::EqState IsoApo::saved_state() const {
    isotone::EqState state;
    const std::wstring dir = isotone::win::persisted_state_dir(kSelftest);
    isotone::ParamBlock block{};
    switch (dir.empty() ? isotone::win::PersistedRead::Invalid
                        : isotone::win::read_persisted_state(isotone::win::persisted_state_path(dir, endpoint_guid_),
                                                             &block)) {
        case isotone::win::PersistedRead::Loaded:
            isotone::from_param_block(block, &state);
            return state;
        case isotone::win::PersistedRead::Invalid:
            debug_line(L"saved state unreadable or not valid for this build; starting flat", E_FAIL);
            return state;
        case isotone::win::PersistedRead::Absent:
            break;
    }
    // Never saved: flat. The self-test build starts with a -12 dB dip at 1 kHz
    // instead, which its measurements look for.
    if (kSelftest) {
        isotone::Band band;
        band.id = 1;
        band.type = isotone::FilterType::Peaking;
        band.fc = 1000.0;
        band.gain_db = -12.0;
        band.width = 1.0;
        band.width_mode = isotone::WidthMode::Q;
        state.bands.push_back(band);
    }
    return state;
}

HRESULT IsoApo::IsInputFormatSupported(IAudioMediaType* output, IAudioMediaType* requested,
                                       IAudioMediaType** supported) {
    if (requested == nullptr) {
        return E_POINTER;
    }

    // With a child, the child decides: it may convert the format, and IsoAPO
    // processes whatever it outputs. A child that refuses is dropped and IsoAPO
    // answers alone, as upstream EqualizerAPO::IsInputFormatSupported does, so
    // the endpoint keeps its EQ. This includes the probe the base class's
    // LockForProcess makes after the child has locked, so the child is unlocked
    // first. While locked the audio thread may be running the child, so it stays.
    if (child_ != nullptr) {
        const HRESULT hr = child_->IsInputFormatSupported(output, requested, supported);
        if (SUCCEEDED(hr) || locked_) {
            return hr;
        }
        debug_line(L"child APO refused a format; running without it", hr);
        if (child_locked_) {
            child_cfg_->UnlockForProcess();
            child_locked_ = false;
        }
        reset_child();
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
    // First, before the child is touched: the audio thread may be using it.
    if (locked_) {
        return APOERR_APO_LOCKED;
    }
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

    // Upstream sizes by the output's count when the input gives none.
    const UINT32 max_frames = inputs[0]->u32MaxFrameCount != 0 ? inputs[0]->u32MaxFrameCount
                                                               : outputs[0]->u32MaxFrameCount;
    if (max_frames == 0) {
        return E_INVALIDARG;
    }

    if (child_cfg_ != nullptr) {
        hr = child_cfg_->LockForProcess(inputCount, inputs, outputCount, outputs);
        if (FAILED(hr) && format.dwSamplesPerFrame != out_format.dwSamplesPerFrame) {
            // The child accepted a channel change IsoAPO cannot make alone, so
            // there is nothing to run without it. The child is kept: a later
            // lock may succeed.
            debug_line(L"child APO failed LockForProcess on a format only it can convert", hr);
            return hr;
        }
        if (FAILED(hr)) {
            debug_line(L"child APO failed LockForProcess; running without it", hr);
            reset_child();
        } else {
            child_locked_ = true;
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

    // Asks IsInputFormatSupported again, which drops a child that refuses now.
    // A dropped child's channel change then fails here, in IsoAPO's own answer.
    hr = CBaseAudioProcessingObject::LockForProcess(inputCount, inputs, outputCount, outputs);
    if (FAILED(hr)) {
        if (child_locked_) {
            child_cfg_->UnlockForProcess();
            child_locked_ = false;
        }
        return hr;
    }

    // From here on a failure must leave nothing locked.
    struct Undo {
        IsoApo* apo;
        bool armed = true;
        ~Undo() {
            if (!armed) return;
            if (apo->child_locked_) {
                apo->child_cfg_->UnlockForProcess();
                apo->child_locked_ = false;
            }
            apo->CBaseAudioProcessingObject::UnlockForProcess();
        }
    } undo{this};

    // Windows tears down and recreates an APO whenever the device format
    // changes, so every LockForProcess is a cold start (plan 5.4).
    channels_ = format.dwSamplesPerFrame;
    UNCOMPRESSEDAUDIOFORMAT in_format{};
    input_channels_ = SUCCEEDED(inputs[0]->pFormat->GetUncompressedAudioFormat(&in_format))
                          ? in_format.dwSamplesPerFrame
                          : channels_;
    // Upstream EqualizerAPO::LockForProcess, render side: the output format's
    // mask, or the input's when the output reports none and the channel counts
    // match. A stream that reports no mask gets upstream's default for its
    // channel count, which is also what the processor assumes for 0.
    uint32_t speaker_mask = out_format.dwChannelMask;
    if (speaker_mask == 0 && in_format.dwSamplesPerFrame == out_format.dwSamplesPerFrame) {
        speaker_mask = in_format.dwChannelMask;
    }
    if (speaker_mask == 0) {
        speaker_mask = isotone::default_speaker_mask(channels_);
    }
    sample_rate_ = static_cast<uint32_t>(format.fFramesPerSecond);
    speaker_mask_ = speaker_mask;
    // A region Initialize could not make or open is tried again here.
    if (!mapping_.is_open()) {
        open_shared_region();
    }
    if (!mapping_.is_open()) {
        state_ = saved_state();
    } else if (isotone::param_block_read(mapping_.params(), &block_, 1000)) {
        isotone::from_param_block(block_, &state_);
        applied_seq_ = block_.hdr.seq;
    } else {
        // No consistent read: a writer is mid-write, or died mid-write and left
        // the lock taken. Keep the current state, which is the last block this
        // instance applied or the saved state open_shared_region read, and make
        // the first process call try the region again.
        applied_seq_ = ~isotone::param_block_seq(mapping_.params());
    }
    // from_param_block reuses this capacity, so applying a block on the audio
    // thread never allocates.
    state_.bands.reserve(isotone::kParamMaxBands);
    // Per-channel values follow their speaker onto this stream's layout when the
    // state was written for another.
    isotone::remap_channels(&state_, isotone::ChannelLayout{channels_, speaker_mask_});

    processor_.initialize(format.fFramesPerSecond, channels_, max_frames, isotone::kParamMaxBands, speaker_mask);
    processor_.set_target(state_);
    processor_.reset();

    if (mapping_.is_open()) {
        ring_.set_channels(channels_);
        // Here rather than on the audio thread, whose first lap through a
        // pagefile-backed ring would fault every page in.
        ring_.prefault();
        claim_ring();
    }

    undo.armed = false;
    locked_ = true;
    return S_OK;
}

// The header's format is the ring owner's: the UI draws the ring's spectrum at
// that rate and parses imports for that layout. Every instance on the endpoint
// locks (a 16 kHz communications stream beside 48 kHz media), but only the
// owner publishes. Real-time safe.
void IsoApo::claim_ring() {
    if (ring_.claim(ring_token_) && ring_.owns()) {
        isotone::host_publish_format(mapping_.params(), sample_rate_, channels_, speaker_mask_,
                                     isotone::HostState::Running);
    }
}

HRESULT IsoApo::UnlockForProcess() {
    // First, so a second unlock does not reach the child: it would be unbalanced.
    if (!locked_) {
        return APOERR_ALREADY_UNLOCKED;
    }
    locked_ = false;
    ring_.release();
    if (child_locked_) {
        child_locked_ = false;
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
            isotone::remap_channels(&state_, isotone::ChannelLayout{channels_, speaker_mask_});
            processor_.set_target(state_);
        }
        // Another instance may have released the ring since this one locked.
        if (!ring_.owns()) {
            claim_ring();
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
        // The flags read back below are the child's only if it sets them; a
        // child that does not must not inherit what this call wrote last time.
        outputs[0]->u32BufferFlags = static_cast<APO_BUFFER_FLAGS>(flags);
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
    // the next sound. The processor takes at most the negotiated maximum a
    // call, and a longer buffer is processed in pieces rather than skipped.
    const UINT32 piece = processor_.max_frames();
    for (UINT32 done = 0; done < frames; done += piece) {
        processor_.process_interleaved(out + static_cast<size_t>(done) * channels_,
                                       frames - done < piece ? frames - done : piece);
    }
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
