// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-measure's window accounting and analysis, driven through fake audio
// clients: a render client whose frames reappear on a capture client, as on
// VB-Cable, with failures injected per call. No device is opened.

#include "measure.h"
// measure.h's HRESULT CHECK would shadow doctest's.
#undef CHECK

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

using namespace measure;

namespace {

// Calls fail with a device-invalidated error on ticks in [from, to).
struct Fail {
    uint64_t from = UINT64_MAX;
    uint64_t to   = UINT64_MAX;
    bool at(uint64_t tick) const { return tick >= from && tick < to; }
};

// The loop between the two fake clients. Each tick is 10 ms: the engine plays
// that much of the render queue, or zeros where the queue ran dry, and the
// capture side records it, as the cable does.
struct Cable {
    uint32_t rate = 48000;
    uint32_t channels = 2;
    uint32_t mask = 0x3;
    std::vector<float> gains;       // per channel on the way through
    bool capture_records = true;    // false: the capture endpoint delivers nothing

    Fail padding, render_get, render_release, packet_size, capture_get, capture_release;
    Fail discontinuity;             // packets flagged as discontinuous
    Fail dropout;                   // the cable plays zeros, unflagged: a splice

    uint64_t tick = 0;
    std::deque<float> queue;        // rendered, not yet played (interleaved)
    std::deque<float> recorded;     // played, not yet captured (interleaved)
    std::vector<float> render_buffer;
    std::vector<float> packet;

    UINT32 frames_per_tick() const { return rate / 100; }

    void advance() {
        for (UINT32 n = 0; n < frames_per_tick(); ++n) {
            for (uint32_t c = 0; c < channels; ++c) {
                float v = 0.0f;
                if (!queue.empty()) {
                    v = queue.front();
                    queue.pop_front();
                }
                if (c < gains.size()) v *= gains[c];
                if (dropout.at(tick)) v = 0.0f;
                if (capture_records) recorded.push_back(v);
            }
        }
        ++tick;
    }
};

constexpr HRESULT kInvalidated = AUDCLNT_E_DEVICE_INVALIDATED;

struct FakeRenderClient : IAudioRenderClient {
    Cable& cable;
    explicit FakeRenderClient(Cable& c) : cable(c) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override {
        if (cable.render_get.at(cable.tick)) return kInvalidated;
        cable.render_buffer.assign(static_cast<size_t>(frames) * cable.channels, 0.0f);
        *data = reinterpret_cast<BYTE*>(cable.render_buffer.data());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD) override {
        if (cable.render_release.at(cable.tick)) return kInvalidated;
        cable.queue.insert(cable.queue.end(), cable.render_buffer.begin(),
                           cable.render_buffer.begin() + static_cast<ptrdiff_t>(frames) * cable.channels);
        return S_OK;
    }
};

struct FakeCaptureClient : IAudioCaptureClient {
    Cable& cable;
    explicit FakeCaptureClient(Cable& c) : cable(c) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    UINT32 available() const {
        return std::min<UINT32>(static_cast<UINT32>(cable.recorded.size() / cable.channels), cable.frames_per_tick());
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64*, UINT64*) override {
        if (cable.capture_get.at(cable.tick)) return kInvalidated;
        *frames = available();
        cable.packet.assign(cable.recorded.begin(),
                            cable.recorded.begin() + static_cast<ptrdiff_t>(*frames) * cable.channels);
        *data = reinterpret_cast<BYTE*>(cable.packet.data());
        *flags = cable.discontinuity.at(cable.tick) ? AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames) override {
        if (cable.capture_release.at(cable.tick)) return kInvalidated;
        cable.recorded.erase(cable.recorded.begin(),
                             cable.recorded.begin() + static_cast<ptrdiff_t>(frames) * cable.channels);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) override {
        if (cable.packet_size.at(cable.tick)) return kInvalidated;
        *frames = available();
        return S_OK;
    }
};

struct FakeClient : IAudioClient {
    Cable& cable;
    FakeRenderClient render;
    FakeCaptureClient capture;
    explicit FakeClient(Cable& c) : cable(c), render(c), capture(c) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                         const WAVEFORMATEX*, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
        *frames = cable.rate / 5;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* frames) override {
        if (cable.padding.at(cable.tick)) return kInvalidated;
        *frames = static_cast<UINT32>(cable.queue.size() / cable.channels);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        auto* f = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f->Format.nChannels = static_cast<WORD>(cable.channels);
        f->Format.nSamplesPerSec = cable.rate;
        f->Format.wBitsPerSample = 32;
        f->Format.nBlockAlign = static_cast<WORD>(4 * cable.channels);
        f->Format.nAvgBytesPerSec = cable.rate * f->Format.nBlockAlign;
        f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f->Samples.wValidBitsPerSample = 32;
        f->dwChannelMask = cable.mask;
        f->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        *format = &f->Format;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** out) override {
        if (IsEqualIID(riid, __uuidof(IAudioRenderClient))) {
            *out = static_cast<IAudioRenderClient*>(&render);
            return S_OK;
        }
        if (IsEqualIID(riid, __uuidof(IAudioCaptureClient))) {
            *out = static_cast<IAudioCaptureClient*>(&capture);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
};

// Render and capture on one cable, started, with the tool's default window.
struct Rig {
    Cable cable;
    FakeClient render_client{cable};
    FakeClient capture_client{cable};
    RenderStream render;
    CaptureStream capture;
    double phase = 0.0;

    explicit Rig(uint32_t channels = 2, uint32_t mask = 0x3) {
        cable.channels = channels;
        cable.mask = mask;
        render.open(&render_client);
        capture.open(&capture_client, false);
        render.start();
        capture.start();
    }

    static WindowPlan plan(double frequency = 1000.0) {
        WindowPlan p;
        p.frequency = frequency;
        p.amplitude = 0.25;
        p.settle_ticks = 30;
        p.measure_ticks = 30;
        p.frames_required = frames_required(0.30, 48000);
        return p;
    }

    FrequencyResult measure(const WindowPlan& p, int phase_reference = -1) {
        FrequencyResult r;
        r.frequency = p.frequency;
        r.window = take_window(render, capture, p, &phase, [this] { cable.advance(); });
        r.channels = analyse(r.window, capture.format().channels, p.frequency, capture.format().sample_rate,
                             p.amplitude, phase_reference);
        return r;
    }

    MeasureReport report() const {
        MeasureReport rep;
        rep.render_id = "render";
        rep.capture_id = "capture";
        rep.render = render.format();
        rep.capture = capture.format();
        rep.amplitude = 0.25;
        rep.frames_required = frames_required(0.30, 48000);
        rep.stream_errors = render.errors() + capture.errors();
        return rep;
    }
};

std::string json_of(const MeasureReport& report) {
    std::FILE* f = nullptr;
    REQUIRE(tmpfile_s(&f) == 0);
    write_json(f, report);
    std::rewind(f);
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    // For checking the output with a real JSON parser.
    char dir[MAX_PATH];
    const DWORD len = GetEnvironmentVariableA("ISOTONE_MEASURE_JSON_DIR", dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        static int serial = 0;
        std::ofstream(std::string(dir) + "/report" + std::to_string(serial++) + ".json", std::ios::binary) << out;
    }
    return out;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("a clean window on the cable measures the tone") {
    Rig rig;
    const FrequencyResult r = rig.measure(Rig::plan());
    const WindowResult& w = r.window;
    CHECK_FALSE(w.failed());
    CHECK(w.attempts == 1);
    CHECK(w.stream_errors == 0);
    CHECK(w.glitches == 0);
    CHECK(w.frames_captured == 14400);
    CHECK(w.frames_rendered >= 14400);
    for (uint32_t c = 0; c < 2; ++c) {
        CHECK(r.channels.magnitude_db[c] == doctest::Approx(20.0 * std::log10(0.25)).epsilon(1e-4));
    }
    CHECK(r.channels.phase_reference == 0);
    CHECK(r.channels.phase_deg[0] == doctest::Approx(0.0));
    CHECK(r.channels.phase_deg[1] == doctest::Approx(0.0));

    MeasureReport rep = rig.report();
    rep.results.push_back(r);
    CHECK(rep.ok());
    CHECK(exit_code(rep) == 0);
}

TEST_CASE("a failed render or capture call fails the window instead of reading as silence") {
    struct Scenario {
        const char* name;
        Fail Cable::*call;
    };
    const Scenario scenarios[] = {
        {"render GetCurrentPadding", &Cable::padding},
        {"render GetBuffer", &Cable::render_get},
        {"render ReleaseBuffer", &Cable::render_release},
        {"capture GetNextPacketSize", &Cable::packet_size},
        {"capture GetBuffer", &Cable::capture_get},
        {"capture ReleaseBuffer", &Cable::capture_release},
    };
    for (const Scenario& s : scenarios) {
        CAPTURE(s.name);
        // Dead from the start: an invalidated stream.
        {
            Rig rig;
            (rig.cable.*s.call).from = 0;
            const FrequencyResult r = rig.measure(Rig::plan());
            CHECK(r.window.stream_error);
            CHECK(r.window.failed());
            CHECK(r.window.attempts == 3);
            CHECK(r.window.stream_errors >= 3);
            CHECK(std::isnan(r.channels.magnitude_db[0]));
            CHECK(std::isnan(r.channels.magnitude_db[1]));

            MeasureReport rep = rig.report();
            rep.results.push_back(r);
            CHECK_FALSE(rep.ok());
            CHECK(exit_code(rep) == kExitWindowFailed);
            CHECK(rep.stream_errors >= 3);
            const std::string json = json_of(rep);
            CHECK(contains(json, "\"ok\": false"));
            CHECK(contains(json, "\"reasons\": [\"stream_error\""));
            CHECK(contains(json, "\"magnitude_db\": [\n    [null],\n    [null]\n  ]"));
        }
        // Invalidated in the middle of the first window's measurement.
        {
            Rig rig;
            (rig.cable.*s.call).from = 45;
            const FrequencyResult r = rig.measure(Rig::plan());
            CHECK(r.window.stream_error);
            CHECK(r.window.failed());
        }
    }
}

TEST_CASE("a window that captured nothing fails, including a zero-length window") {
    CHECK(frames_required(0.30, 48000) == 14400);
    CHECK(frames_required(0.0, 48000) == kMinWindowFrames);

    Rig rig;
    rig.cable.capture_records = false;
    WindowPlan p = Rig::plan();
    p.measure_ticks = 0;
    p.frames_required = frames_required(0.0, 48000);
    const FrequencyResult r = rig.measure(p);
    CHECK(r.window.frames_captured == 0);
    CHECK_FALSE(r.window.stream_error);
    CHECK(r.window.short_capture);
    CHECK(r.window.failed());
    CHECK(std::isnan(r.channels.magnitude_db[0]));

    MeasureReport rep = rig.report();
    rep.results.push_back(r);
    CHECK(exit_code(rep) == kExitWindowFailed);
    CHECK(contains(json_of(rep), "\"reasons\": [\"short_capture\"]"));

    // The same zero-length window on a working cable takes the minimum and passes.
    Rig ok;
    const FrequencyResult good = ok.measure(p);
    CHECK_FALSE(good.window.failed());
    CHECK(good.window.frames_captured >= kMinWindowFrames);
}

TEST_CASE("a transient stream error is retaken and still reported") {
    Rig rig;
    rig.cable.padding.from = 5;
    rig.cable.padding.to = 6;
    const FrequencyResult r = rig.measure(Rig::plan());
    CHECK(r.window.attempts == 2);
    CHECK(r.window.stream_errors == 1);
    CHECK_FALSE(r.window.failed());

    MeasureReport rep = rig.report();
    rep.results.push_back(r);
    CHECK(rep.ok());
    const std::string json = json_of(rep);
    CHECK(contains(json, "\"stream_errors\": [1]"));
    CHECK(contains(json, "\"stream_errors_total\": 1"));
    CHECK(contains(json, "\"failed_windows\": []"));
}

TEST_CASE("a window still glitched after its retakes fails the run and is named") {
    Rig rig;
    MeasureReport rep;
    rep.results.push_back(rig.measure(Rig::plan(1000.0)));
    const uint64_t second_starts = rig.cable.tick;
    rig.cable.discontinuity.from = second_starts;
    rep.results.push_back(rig.measure(Rig::plan(2000.0)));
    rig.cable.discontinuity = Fail{};
    rep.results.push_back(rig.measure(Rig::plan(4000.0)));

    CHECK_FALSE(rep.results[0].window.failed());
    CHECK(rep.results[1].window.glitched);
    CHECK(rep.results[1].window.attempts == 3);
    CHECK_FALSE(rep.results[2].window.failed());

    const MeasureReport base = rig.report();
    rep.render = base.render;
    rep.capture = base.capture;
    CHECK_FALSE(rep.ok());
    CHECK(exit_code(rep) == kExitWindowFailed);
    const std::string json = json_of(rep);
    CHECK(contains(json, "\"failed\": [false, true, false]"));
    CHECK(contains(json, "\"failed_windows\": [\n    {\"index\": 1, \"frequency\": 2000.000000, \"reasons\": [\"glitch\"]}\n  ]"));
    // The reported attempt is the third; the first two are the discarded ones.
    REQUIRE(rep.results[1].window.discarded.size() == 2);
    CHECK(rep.results[1].window.discarded[0].attempt == 1);
    CHECK(rep.results[1].window.discarded[1].attempt == 2);
    CHECK(contains(json, "\"discarded_attempts\": [[], [{\"attempt\": 1, \"reasons\": [\"glitch\"]"));
}

TEST_CASE("a retaken window reports the attempts it discarded and why") {
    // Settle is ticks 0 to 29 and the first window 30 to 59.
    const auto attempt_json = [](const DiscardedAttempt& d, const char* reasons) {
        return "{\"attempt\": " + std::to_string(d.attempt) + ", \"reasons\": [" + reasons +
               "], \"glitches\": " + std::to_string(d.glitches) +
               ", \"discontinuities\": " + std::to_string(d.discontinuities) +
               ", \"underruns\": " + std::to_string(d.underruns) +
               ", \"stream_errors\": " + std::to_string(d.stream_errors) +
               ", \"frames_rendered\": " + std::to_string(d.frames_rendered) +
               ", \"frames_captured\": " + std::to_string(d.frames_captured) +
               ", \"residual_re_fit_db\": " + json_number(d.residual_re_fit_db) + "}";
    };

    SUBCASE("a glitch, then a pass") {
        Rig rig;
        rig.cable.discontinuity = Fail{40, 41};
        const FrequencyResult r = rig.measure(Rig::plan());
        CHECK(r.window.attempts == 2);
        CHECK_FALSE(r.window.failed());
        CHECK(r.window.glitches == 0);
        CHECK(r.window.stream_errors == 0);
        CHECK(r.window.frames_captured == 14400);

        REQUIRE(r.window.discarded.size() == 1);
        const DiscardedAttempt& d = r.window.discarded[0];
        CHECK(d.attempt == 1);
        CHECK(d.glitched);
        CHECK_FALSE(d.stream_error);
        CHECK_FALSE(d.short_capture);
        CHECK(d.glitches == 1);
        CHECK(d.discontinuities == 1);
        CHECK(d.underruns == 0);
        CHECK(d.stream_errors == 0);
        CHECK(d.frames_captured == 14400);
        CHECK(d.residual_re_fit_db < -40.0);   // flagged, not spliced

        MeasureReport rep = rig.report();
        rep.results.push_back(r);
        CHECK(rep.ok());
        const std::string json = json_of(rep);
        CHECK(contains(json, "\"discarded_attempts\": [[" + attempt_json(d, "\"glitch\"") + "]],\n"));
        CHECK(contains(json, "\"glitches\": [0]"));
        CHECK(contains(json, "\"attempts\": [2]"));
        CHECK(contains(json, "\"failed_windows\": []"));
    }

    SUBCASE("a stream error, then a pass") {
        Rig rig;
        rig.cable.padding = Fail{5, 6};
        const FrequencyResult r = rig.measure(Rig::plan());
        CHECK(r.window.attempts == 2);
        CHECK_FALSE(r.window.failed());
        CHECK(r.window.glitches == 0);

        REQUIRE(r.window.discarded.size() == 1);
        const DiscardedAttempt& d = r.window.discarded[0];
        CHECK(d.attempt == 1);
        CHECK(d.stream_error);
        CHECK(d.stream_errors == 1);
        // The error ended the attempt in its settle, so its window captured nothing.
        CHECK(d.short_capture);
        CHECK(d.frames_captured == 0);
        CHECK_FALSE(d.glitched);
        CHECK(std::isnan(d.residual_re_fit_db));

        MeasureReport rep = rig.report();
        rep.results.push_back(r);
        const std::string json = json_of(rep);
        CHECK(contains(json, "\"discarded_attempts\": [[" + attempt_json(d, "\"stream_error\", \"short_capture\"") +
                                 "]],\n"));
        CHECK(contains(json, "\"residual_re_fit_db\": null}"));
        CHECK(contains(json, "\"failed_windows\": []"));
    }

    SUBCASE("an unflagged splice, then a pass") {
        Rig rig;
        rig.cable.dropout = Fail{40, 41};
        const FrequencyResult r = rig.measure(Rig::plan());
        CHECK(r.window.attempts == 2);
        CHECK_FALSE(r.window.failed());

        REQUIRE(r.window.discarded.size() == 1);
        const DiscardedAttempt& d = r.window.discarded[0];
        CHECK(d.glitched);
        CHECK(d.glitches == 1);
        CHECK(d.discontinuities == 0);
        CHECK(d.underruns == 0);
        // 10 ms of zeros in a 300 ms window: over the splice threshold, well under the tone.
        CHECK(d.residual_re_fit_db > -40.0);
        CHECK(d.residual_re_fit_db < -6.0);

        MeasureReport rep = rig.report();
        rep.results.push_back(r);
        CHECK(contains(json_of(rep), "\"discarded_attempts\": [[" + attempt_json(d, "\"glitch\"") + "]],\n"));
    }

    SUBCASE("no retakes: empty lists") {
        Rig rig;
        MeasureReport rep = rig.report();
        rep.results.push_back(rig.measure(Rig::plan(1000.0)));
        rep.results.push_back(rig.measure(Rig::plan(2000.0)));
        for (const FrequencyResult& f : rep.results) {
            CHECK(f.window.attempts == 1);
            CHECK(f.window.discarded.empty());
        }
        CHECK(contains(json_of(rep), "\"discarded_attempts\": [[], []],\n"));
    }
}

TEST_CASE("phase needs a reference carrying the tone") {
    const auto sine = [](double amplitude, double phase_deg) {
        std::vector<float> x(4800);
        for (size_t i = 0; i < x.size(); ++i) {
            x[i] = static_cast<float>(amplitude * std::cos(2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0 +
                                                           phase_deg * kPi / 180.0));
        }
        return x;
    };
    WindowResult w;
    w.captured = {std::vector<float>(4800, 0.0f), sine(0.1, 0.0), sine(0.2, 180.0)};

    SUBCASE("channel 0 silent: every phase is null against it, not 0") {
        const ChannelResults r = analyse(w, 3, 1000.0, 48000.0, 0.25, 0);
        CHECK(r.phase_reference == -1);
        for (double p : r.phase_deg) CHECK(std::isnan(p));
    }
    SUBCASE("by default the strongest carrying channel is the reference when channel 0 is silent") {
        const ChannelResults r = analyse(w, 3, 1000.0, 48000.0, 0.25, -1);
        CHECK(r.phase_reference == 2);
        CHECK(std::isnan(r.phase_deg[0]));
        CHECK(std::abs(r.phase_deg[1]) == doctest::Approx(180.0).epsilon(1e-6));
        CHECK(r.phase_deg[2] == doctest::Approx(0.0));
    }
    SUBCASE("by default channel 0 stays the reference while it carries the tone") {
        WindowResult v = w;
        v.captured[0] = sine(0.05, 90.0);
        const ChannelResults r = analyse(v, 3, 1000.0, 48000.0, 0.25, -1);
        CHECK(r.phase_reference == 0);
        CHECK(r.phase_deg[1] == doctest::Approx(-90.0).epsilon(1e-6));
        CHECK(r.phase_deg[2] == doctest::Approx(90.0).epsilon(1e-6));
    }
    SUBCASE("a chosen reference is used") {
        const ChannelResults r = analyse(w, 3, 1000.0, 48000.0, 0.25, 1);
        CHECK(r.phase_reference == 1);
        CHECK(std::abs(r.phase_deg[2]) == doctest::Approx(180.0).epsilon(1e-6));
        CHECK(std::isnan(r.phase_deg[0]));
    }
    SUBCASE("the JSON says null and names the reference") {
        MeasureReport rep;
        rep.capture.channels = 3;
        FrequencyResult f;
        f.frequency = 1000.0;
        f.window = w;
        f.channels = analyse(w, 3, 1000.0, 48000.0, 0.25, 0);
        rep.results.push_back(f);
        f.channels = analyse(w, 3, 1000.0, 48000.0, 0.25, -1);
        rep.results.push_back(f);
        const std::string json = json_of(rep);
        CHECK(contains(json, "\"phase_reference\": [null, 2]"));
        CHECK(contains(json, "\"phase_deg\": [\n    [null, null],"));
        CHECK_FALSE(contains(json, "\"phase_deg\": [\n    [0.000000"));
    }
}

TEST_CASE("render and capture layouts are recorded and a channel count mismatch is refused") {
    StreamFormat stereo;
    stereo.sample_rate = 48000;
    stereo.channels = 2;
    stereo.bits = 24;
    stereo.frame_bytes = 6;
    stereo.channel_mask = 0x3;
    StreamFormat surround = stereo;
    surround.channels = 8;
    surround.frame_bytes = 24;
    surround.channel_mask = 0x63F;

    CHECK_FALSE(layout_mismatch(stereo, surround, false).empty());
    CHECK_FALSE(layout_mismatch(surround, stereo, false).empty());
    CHECK(layout_mismatch(stereo, surround, true).empty());
    CHECK(layout_mismatch(surround, surround, false).empty());

    Rig rig(8, 0x63F);
    CHECK(rig.render.format().channels == 8);
    CHECK(rig.capture.format().channel_mask == 0x63F);
    MeasureReport rep = rig.report();
    rep.render = surround;
    const std::string json = json_of(rep);
    CHECK(contains(json, "\"render_format\": {\"sample_rate\": 48000, \"channels\": 8, \"sample_format\": \"int\", "
                         "\"bits\": 24, \"frame_bytes\": 24, \"channel_mask\": 1599}"));
    CHECK(contains(json, "\"capture_format\": {\"sample_rate\": 48000, \"channels\": 8, \"sample_format\": \"float\", "
                         "\"bits\": 32, \"frame_bytes\": 32, \"channel_mask\": 1599}"));
}

TEST_CASE("a settle too short to clear VB-Cable's replayed frame is refused") {
    // VB-Cable replays one old frame 20 to 55 ms after the later of the capture
    // and render starts (2026-09-14); a settle of 0 put it in the first window.
    CHECK_FALSE(settle_is_enough(0.0));
    CHECK_FALSE(settle_is_enough(0.05));
    CHECK_FALSE(settle_is_enough(-1.0));
    CHECK_FALSE(settle_is_enough(std::nan("")));
    CHECK(settle_is_enough(0.1));
    CHECK(settle_is_enough(0.3));
}

TEST_CASE("an endpoint fragment must name one endpoint") {
    const std::vector<Endpoint> list = {
        {"{0.0.0.00000000}.{798436d2-8c71-4834-9248-00ccbaaca00a}", "VB-Audio Virtual Cable", "CABLE Input"},
        {"{0.0.0.00000000}.{3a0f5b1e-0000-4000-8000-000000000001}", "VB-Audio Virtual Cable", "CABLE In 16ch"},
        {"{0.0.0.00000000}.{00000000-1111-4000-8000-000000000002}", "Realtek(R) Audio", "Speakers"},
    };
    CHECK(match_endpoints(list, "{0.0.0.00000000}").size() == 3);
    CHECK(match_endpoints(list, "4000-8000").size() == 2);
    REQUIRE(match_endpoints(list, "{798436D2").size() == 1);
    CHECK(match_endpoints(list, "{798436D2")[0] == &list[0]);
    CHECK(match_endpoints(list, list[1].id).size() == 1);
    CHECK(match_endpoints(list, "cable in").size() == 2);
    REQUIRE(match_endpoints(list, "16ch").size() == 1);
    CHECK(match_endpoints(list, "16ch")[0] == &list[1]);
    CHECK(match_endpoints(list, "nothing").empty());
}

TEST_CASE("non-ASCII arguments reach the JSON as UTF-8") {
    wchar_t arg0[] = L"isotone-measure";
    wchar_t arg1[] = L"--label";
    // "café Ω "🎵"" as UTF-16 code units. Written out, since MSVC without /utf-8
    // turns é in a wide literal into two characters.
    wchar_t arg2[] = {L'c', L'a', L'f', 0x00E9, L' ', 0x03A9, L' ', L'"', 0xD83C, 0xDFB5, L'"', 0};
    wchar_t* argv[] = {arg0, arg1, arg2};
    const std::vector<std::string> args = utf8_args(3, argv);
    REQUIRE(args.size() == 3);
    const std::string expected = "caf\xC3\xA9 \xCE\xA9 \"\xF0\x9F\x8E\xB5\"";
    CHECK(args[2] == expected);
    CHECK(json_string(args[2]) == "\"caf\xC3\xA9 \xCE\xA9 \\\"\xF0\x9F\x8E\xB5\\\"\"");

    MeasureReport rep;
    rep.label = args[2];
    rep.render_id = "{0.0.0.00000000}.{\xE2\x82\xAC}";
    CHECK(contains(json_of(rep), "\"label\": \"caf\xC3\xA9 \xCE\xA9 \\\"\xF0\x9F\x8E\xB5\\\"\""));
}
