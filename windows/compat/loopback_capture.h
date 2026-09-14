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

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "isotone/audio_ring.h"

namespace isotone::compat {

// Whether a captured packet's flags mark a glitch. The discontinuity flag means
// "the data in the packet is not correlated with the previous packet's device
// position; this is possibly due to a stream state transition or timing glitch"
// (Microsoft Learn, _AUDCLNT_BUFFERFLAGS). The first packet after
// IAudioClient::Start has no previous packet and follows a state transition, and
// Windows 7 and later flag it: Matthew van Eerde's WASAPI loopback-capture sample
// ignores the flag there "since Windows 7 sets it", and Chromium's WASAPI input
// notes the flag "is undefined on the application's first call to GetBuffer
// after Start". So it is not counted on that packet.
bool counts_as_glitch(DWORD flags, bool first_packet_after_start);

// Threading: start() and stop() from one thread at a time. read() and the other
// accessors from any thread, including while stop() runs on another: the ring
// stays allocated until the last of them returns.
class LoopbackCapture {
public:
    LoopbackCapture() = default;
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    ~LoopbackCapture() { stop(); }

    // `endpoint` is a render endpoint's GUID, with or without braces, or its full
    // device id. Waits up to `timeout_ms` for the stream to run or fail. On
    // HRESULT_FROM_WIN32(ERROR_TIMEOUT) the capture thread, blocked in a call to
    // the audio service, is left to end by itself when that call returns.
    HRESULT start(const std::string& endpoint, DWORD timeout_ms = 5000);
    void stop();

    bool running() const;
    uint32_t sample_rate() const;
    uint32_t channels() const;   // of the stream; read() gives at most kMaxChannels
    HRESULT thread_error() const;
    // Packets WASAPI flagged as not continuous with the one before: a glitch in
    // what was captured. Counted from start(), as counts_as_glitch says.
    uint32_t discontinuities() const;

    // As audio_ring_read. `out` holds max_frames * kMaxChannels samples.
    uint32_t read(AudioRingCursor* cursor, float* out, uint32_t max_frames, uint32_t* channels) const;

private:
    // What the capture thread uses, shared with it so a thread start() gave up
    // on can outlive this object.
    struct State;
    std::shared_ptr<State> state() const;
    static void run(std::shared_ptr<State> state, std::wstring device_id);

    std::thread thread_;
    mutable std::mutex mutex_;       // guards state_ itself, not what it points to
    std::shared_ptr<State> state_;
};

}  // namespace isotone::compat
