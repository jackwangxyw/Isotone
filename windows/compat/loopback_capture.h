// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Spectrum source for the compatibility backend (plan 5.5): WASAPI loopback on
// a render endpoint. A stock Equalizer APO has no audio ring, so this stands in
// for one, and it is read through the same contract as audio_ring_read: a
// cursor, the newest frames when more are waiting than fit, dropped frames
// rather than torn ones. Internally it is an AudioRing on the heap, written by
// a capture thread.
//
// Where loopback taps, measured on the VB-Cable render endpoint with IsoAPO in
// MFX: a steady tone captured at the same time from IsoAPO's ring and from
// loopback differed by -0.052 dB at 100 Hz and -0.050 dB at 1 kHz, where the
// APO applied 0.16 dB and 12 dB respectively. So loopback is downstream of MFX
// there, with a small flat gain after it. Other endpoints, and EFX, are not
// measured.
//
// On that endpoint loopback also delivered frames while this code played
// nothing to it; whether a truly idle endpoint delivers any is not verified.

#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "isotone/audio_ring.h"

namespace isotone::compat {

class LoopbackCapture {
public:
    LoopbackCapture() = default;
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    ~LoopbackCapture() { stop(); }

    // `endpoint` is a render endpoint's GUID, with or without braces, or its full
    // device id. Blocks until the stream is running or has failed.
    HRESULT start(const std::string& endpoint);
    void stop();

    bool running() const { return running_.load(); }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }   // of the stream; read() gives at most kMaxChannels
    HRESULT thread_error() const { return thread_error_.load(); }

    // As audio_ring_read. `out` holds max_frames * kMaxChannels samples.
    uint32_t read(AudioRingCursor* cursor, float* out, uint32_t max_frames, uint32_t* channels) const;

private:
    void run(std::wstring device_id, HANDLE ready);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<HRESULT> thread_error_{S_OK};
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    void* region_ = nullptr;   // AudioRingHeader then samples
};

}  // namespace isotone::compat
