// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The parts of isotone-measure that decide what a window measured and whether it
// counts: the stream wrappers and their error accounting, the window loop, the
// analysis and the JSON. main.cpp opens devices and parses the command line;
// measure_tests drives these with fake audio clients.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <audioclient.h>

#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace measure {

constexpr double kPi = 3.14159265358979323846;

// A channel whose fitted sine is at or below this amplitude (-80 dBFS) is not
// carrying the tone: it gets no residual check and no phase.
constexpr double kCarryingAmplitude = 1e-4;

// dft_at needs at least this many samples, so no window is shorter.
constexpr uint64_t kMinWindowFrames = 64;

[[noreturn]] void die(const char* what, HRESULT hr);

#define CHECK(expr, what)                                  \
    do {                                                   \
        const HRESULT _hr = (expr);                        \
        if (FAILED(_hr)) ::measure::die((what), _hr);      \
    } while (0)

std::string narrow(const wchar_t* w);
std::wstring widen(const std::string& s);
std::string json_string(const std::string& s);
// A JSON number, or null for what JSON cannot hold.
std::string json_number(double v);

// wmain's arguments as UTF-8, so a non-ASCII label or path reaches the JSON intact.
std::vector<std::string> utf8_args(int argc, wchar_t** argv);

struct Endpoint {
    std::string id;
    std::string name;        // device name, e.g. "VB-Audio Virtual Cable"
    std::string description; // endpoint name, e.g. "CABLE Input"
};

// The endpoints a user-supplied string names. A full id (case-insensitive) is
// one match; otherwise every id containing it, and only when no id does, every
// endpoint whose description or name contains it. More than one is ambiguous.
std::vector<const Endpoint*> match_endpoints(const std::vector<Endpoint>& list, const std::string& query);

// The settle before each window discards what the streams deliver as they start.
// VB-Cable replays one frame of audio rendered while no capture was open, 20 to
// 55 ms after the later of the capture and render starts, so a settle under
// 0.1 s can put that frame in a window.
inline constexpr double kMinSettleSeconds = 0.1;
bool settle_is_enough(double seconds);

// Format description reduced to what the tone generator and analyser need.
struct StreamFormat {
    uint32_t sample_rate  = 0;
    uint32_t channels     = 0;
    bool     is_float     = false;
    uint32_t bits         = 0;
    uint32_t frame_bytes  = 0;
    uint32_t channel_mask = 0;   // 0 when the format is not WAVE_FORMAT_EXTENSIBLE
};

// The sample encodings the tone writer and the capture reader handle.
bool supported(const StreamFormat& f);
StreamFormat describe(const WAVEFORMATEX* wfx);
std::string format_json(const StreamFormat& f);

// Why render and capture cannot be compared channel for channel, or empty when
// they can. A different channel count is refused unless `allow` is set.
std::string layout_mismatch(const StreamFormat& render, const StreamFormat& capture, bool allow);

class RenderStream {
public:
    // Takes ownership of the client.
    void open(IAudioClient* client);
    void start();
    void stop();

    // Channel c plays the tone scaled by gains[c]; channels past the list play
    // it at 1, so an empty list is the same tone everywhere.
    void set_channel_gains(const std::vector<double>& gains) { gains_ = gains; }

    // Tops the queue up to about 50 ms of the tone.
    void pump(double frequency, double amplitude, double* phase);

    const StreamFormat& format() const { return fmt_; }
    uint32_t underruns() const { return underruns_; }
    // Failed calls on the running stream. A stream that was invalidated (the
    // audio service restarted, the format changed) fails every call from then on.
    uint32_t errors() const { return errors_; }
    uint64_t frames() const { return frames_; }

    ~RenderStream();

private:
    void write_sample(BYTE* base, size_t index, double v);

    IAudioClient*       client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    WAVEFORMATEX*       format_ = nullptr;
    StreamFormat        fmt_{};
    UINT32              buffer_frames_ = 0;
    UINT32              lead_frames_ = 0;
    bool                primed_ = false;
    uint32_t            underruns_ = 0;
    uint32_t            errors_ = 0;
    uint64_t            frames_ = 0;
    std::vector<double> gains_;
};

class CaptureStream {
public:
    // Takes ownership of the client.
    void open(IAudioClient* client, bool loopback);
    void start();
    void stop();

    // Drains whatever is available. When `sink` is null the audio is discarded,
    // which is how the settling period is skipped.
    void pump(std::vector<std::vector<float>>* sink);

    const StreamFormat& format() const { return fmt_; }
    // Packets flagged as not continuous with the one before.
    uint32_t discontinuities() const { return discontinuities_; }
    uint32_t errors() const { return errors_; }
    uint64_t frames() const { return frames_; }

    ~CaptureStream();

private:
    float read_sample(const BYTE* base, size_t index) const;

    IAudioClient*        client_  = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    WAVEFORMATEX*        format_  = nullptr;
    StreamFormat         fmt_{};
    uint32_t             discontinuities_ = 0;
    uint32_t             errors_ = 0;
    uint64_t             frames_ = 0;
};

std::complex<double> dft_at(const std::vector<float>& x, double frequency, double sample_rate);
double residual_rms(const std::vector<float>& x, std::complex<double> fit, double frequency, double sample_rate);

struct WindowPlan {
    double frequency = 1000.0;
    double amplitude = 0.25;
    int    settle_ticks = 30;
    int    measure_ticks = 30;
    // A window that captured fewer frames than this failed.
    uint64_t frames_required = kMinWindowFrames;
    int    max_attempts = 3;
};

// Frames a window of `seconds` must capture at `sample_rate`.
uint64_t frames_required(double seconds, uint32_t sample_rate);

// An attempt that failed and was taken again, with the figures that failed it.
struct DiscardedAttempt {
    uint32_t attempt = 0;           // 1 for the first
    bool     stream_error = false;
    bool     short_capture = false;
    bool     glitched = false;
    uint32_t glitches = 0;          // discontinuities + underruns + 1 for a splice
    uint32_t discontinuities = 0;
    uint32_t underruns = 0;
    uint32_t stream_errors = 0;
    uint64_t frames_rendered = 0;
    uint64_t frames_captured = 0;
    // Worst residual re its fitted sine over the channels carrying the tone;
    // above -40 dB is a splice. NaN when no channel carries it.
    double   residual_re_fit_db = 0.0;
};

struct WindowResult {
    std::vector<std::vector<float>> captured;   // [channel][frame], the kept attempt
    uint32_t attempts = 0;
    uint32_t glitches = 0;          // kept attempt: discontinuities, underruns, splices
    uint32_t stream_errors = 0;     // every attempt
    uint64_t frames_rendered = 0;   // kept attempt's window
    uint64_t frames_captured = 0;   // kept attempt's window
    // Why the kept attempt does not count; all false when it does.
    bool stream_error = false;
    bool short_capture = false;
    bool glitched = false;
    std::vector<DiscardedAttempt> discarded;    // the attempts before the kept one

    bool failed() const { return stream_error || short_capture || glitched; }
    // The numbers describe the path: the streams ran and the window is full.
    bool measured() const { return !stream_error && !short_capture; }
};

// Plays plan.frequency, lets it settle, captures a window and retakes it up to
// plan.max_attempts times while it failed. `tick` waits one pump interval.
WindowResult take_window(RenderStream& render, CaptureStream& capture, const WindowPlan& plan,
                         double* phase, const std::function<void()>& tick);

struct ChannelResults {
    std::vector<double> magnitude_db;   // NaN when the window was not measured
    std::vector<double> phase_deg;      // NaN without a carrying reference, or on a channel not carrying the tone
    std::vector<double> residual_db;
    int phase_reference = -1;           // -1: none
};

// Per channel level, phase and residual of `frequency` in a window.
// `phase_reference` is a channel index, or -1 for channel 0 when it carries the
// tone and otherwise the strongest channel that does.
ChannelResults analyse(const WindowResult& window, uint32_t channels, double frequency,
                       double sample_rate, double amplitude, int phase_reference);

struct FrequencyResult {
    double         frequency = 0.0;
    WindowResult   window;              // `captured` is dropped once analysed
    ChannelResults channels;
};

struct MeasureReport {
    std::string  label;
    std::string  render_id;
    std::string  capture_id;
    StreamFormat render;
    StreamFormat capture;
    bool         loopback = false;
    double       amplitude = 0.0;
    uint64_t     frames_required = 0;
    uint32_t     stream_errors = 0;     // whole run
    std::vector<FrequencyResult> results;

    bool ok() const;
};

// Exit status of a completed measurement: 0, or 3 when a window still failed
// after its retakes.
constexpr int kExitWindowFailed = 3;
int exit_code(const MeasureReport& report);

void write_json(std::FILE* out, const MeasureReport& report);

}  // namespace measure
