// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// IsoAPO: the Windows audio processing object that runs the Isotone core inside
// audiodg.exe.
//
// Parameters arrive through the per-endpoint shared region (plan 5.3): the APO
// creates or opens it in Initialize (or, if that failed, in LockForProcess),
// reads the ParamBlock under its seqlock on every process call, writes post-EQ
// audio to the ring, and bumps the heartbeat. When this instance creates the
// region it seeds it from the endpoint's saved state (persisted_state.h), read
// before the region exists so other instances never wait on the file.
//
// The APO an install replaced keeps running inside IsoAPO as its child, as in
// upstream: install records its CLSID, Initialize creates it, format
// negotiation and lock/unlock go to it, and each process call runs it first and
// IsoAPO's processor on its output. A child that fails any step is dropped and
// IsoAPO runs alone.
//
// Structure follows upstream Equalizer APO's EqualizerAPO.cpp, which is the
// known-good shape for this interface, with its FilterEngine replaced by
// isotone::Processor.

#pragma once

#include <windows.h>

#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>

#include <atomic>
#include <string>

#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/types.h"
#include "shared_mapping.h"

// Fresh CLSIDs. Deliberately unrelated to Equalizer APO's, so IsoAPO can be
// installed on a machine that also has a real Equalizer APO without the two
// colliding, and so `devicetool status` can tell them apart.
// {BAF30F18-9FA2-4E55-97D9-007CEA179824}
DEFINE_GUID(ISOAPO_POST_MIX_GUID, 0xbaf30f18, 0x9fa2, 0x4e55, 0x97, 0xd9, 0x00, 0x7c, 0xea, 0x17,
            0x98, 0x24);
// {F1DFFD14-9A30-45C5-BAB2-C820C7EC718F}
DEFINE_GUID(ISOAPO_PRE_MIX_GUID, 0xf1dffd14, 0x9a30, 0x45c5, 0xba, 0xb2, 0xc8, 0x20, 0xc7, 0xec,
            0x71, 0x8f);

class INonDelegatingUnknown {
    virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv) = 0;
    virtual ULONG __stdcall NonDelegatingAddRef() = 0;
    virtual ULONG __stdcall NonDelegatingRelease() = 0;
};

class IsoApo : public CBaseAudioProcessingObject,
               public IAudioSystemEffects,
               public INonDelegatingUnknown {
public:
    explicit IsoApo(IUnknown* outer);
    virtual ~IsoApo();

    // IUnknown
    HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override;
    ULONG __stdcall AddRef() override;
    ULONG __stdcall Release() override;

    // IAudioProcessingObject
    HRESULT __stdcall Reset() override;
    HRESULT __stdcall GetLatency(HNSTIME* time) override;
    HRESULT __stdcall Initialize(UINT32 size, BYTE* data) override;
    HRESULT __stdcall IsInputFormatSupported(IAudioMediaType* output,
                                             IAudioMediaType* requested,
                                             IAudioMediaType** supported) override;

    // IAudioProcessingObjectConfiguration
    HRESULT __stdcall LockForProcess(UINT32 inputCount,
                                     APO_CONNECTION_DESCRIPTOR** inputs,
                                     UINT32 outputCount,
                                     APO_CONNECTION_DESCRIPTOR** outputs) override;
    HRESULT __stdcall UnlockForProcess() override;

    // IAudioProcessingObjectRT
    void __stdcall APOProcess(UINT32 inputCount, APO_CONNECTION_PROPERTY** inputs,
                              UINT32 outputCount, APO_CONNECTION_PROPERTY** outputs) override;

    // INonDelegatingUnknown
    HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv) override;
    ULONG __stdcall NonDelegatingAddRef() override;
    ULONG __stdcall NonDelegatingRelease() override;

    static long instanceCount;
    static const CRegAPOProperties<1> regPostMixProperties;
    static const CRegAPOProperties<1> regPreMixProperties;

private:
    // The endpoint's saved state, or its default when none was saved.
    isotone::EqState saved_state() const;
    void open_shared_region();
    static void seed_region(isotone::ParamBlock* block, void* self);
    HRESULT lock(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
                 APO_CONNECTION_DESCRIPTOR** outputs);
    void claim_ring();
    void create_child(const std::wstring& endpoint_guid, const CLSID& own_clsid, UINT32 size, BYTE* data);
    void reset_child();

    long      refCount_ = 1;
    IUnknown* outer_    = nullptr;

    isotone::Processor processor_;
    isotone::EqState   state_;
    uint32_t           channels_       = 0;   // processed: the output format's
    uint32_t           input_channels_ = 0;
    uint32_t           sample_rate_    = 0;   // published with channels_ and speaker_mask_
    uint32_t           speaker_mask_   = 0;   // while this instance owns the ring
    bool               locked_         = false;
    bool               child_locked_   = false;

    std::wstring                endpoint_guid_;
    isotone::win::SharedMapping mapping_;
    isotone::AudioRingWriter    ring_;
    uint64_t                    ring_token_  = 0;    // process nonce << 32 | instance serial
    uint32_t                    applied_seq_ = 0;    // seq of the block last applied
    isotone::ParamBlock         block_{};            // private copy taken under the seqlock

    IAudioProcessingObject*              child_     = nullptr;
    IAudioProcessingObjectRT*            child_rt_  = nullptr;
    IAudioProcessingObjectConfiguration* child_cfg_ = nullptr;
};
