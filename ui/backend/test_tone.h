// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// A tone on a render endpoint through WASAPI shared mode, so it passes through the
// engine (IsoAPO or Equalizer APO): the per-speaker test tone (PinkNoise on one
// channel, with the speaker's level, delay, polarity and bass management
// applied) and the EQ by ear tone (SineTone on the channels chosen). Channels not
// chosen are silent. The render code follows
// windows/measure's RenderStream: the endpoint's mix format, a 200 ms buffer,
// topped up to about 50 ms every 10 ms.
//
// One thread per tone; start and stop from one owner thread.

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace isotone::ui {

class TestTone {
public:
    // Called on the tone's thread when the stream could not open or failed while
    // playing (a format change invalidates it). `what` names the call.
    using Failure = std::function<void(HRESULT hr, const char* what)>;
    // Fills `frames` mono samples at the endpoint's rate, on the tone's thread.
    using Source = std::function<void(float* out, uint32_t frames, double sample_rate)>;

    TestTone() = default;
    TestTone(const TestTone&) = delete;
    TestTone& operator=(const TestTone&) = delete;
    ~TestTone();

    // Plays `source` on the channels of `endpoint` (anything read_render_endpoint
    // takes) set in `channel_mask`, bit n for channel n, until stop(). Stops a tone
    // already playing first.
    void start(const std::wstring& endpoint, uint32_t channel_mask, Source source, Failure on_failure);
    // Pink noise on `channel`.
    void start(const std::wstring& endpoint, uint32_t channel, Failure on_failure);
    void stop();
    bool playing() const { return thread_.joinable(); }
    // Frames written so far by the current tone.
    uint64_t frames() const { return frames_; }

private:
    void run(std::wstring endpoint, uint32_t channel_mask, Source source, Failure on_failure);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> frames_{0};
};

}  // namespace isotone::ui
