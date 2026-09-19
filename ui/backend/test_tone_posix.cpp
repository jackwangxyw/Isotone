// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The tones on Linux: a PipeWire playback stream into Isotone's virtual sink.
// That sink's monitor is the core's input, so the tone is processed exactly as
// an application's audio is, which is what the Windows half gets by playing on
// the endpoint IsoAPO sits on.
//
// The core runs with one output's state at a time, the one the daemon feeds, and
// its region is named for that output. A tone asked for on another output is
// refused rather than played through the wrong state.

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "isotone/speakers.h"
#include "pink_noise.h"
#include "shared_region.h"
#include "test_tone.h"

namespace isotone::ui {

namespace {

// node.name of the daemon's virtual sink (linux/daemon, Options::sink_name).
constexpr const char* kIsotoneSinkName = "isotone";

// The PipeWire position of each speaker bit, in the order the core numbers
// channels (the n-th set bit is channel n). The daemon names its sink's channels
// the same way, so a stream carrying these positions is not remixed on its way in.
struct PositionBit {
    uint32_t bit;
    uint32_t position;
};
constexpr PositionBit kPositions[] = {
    {kSpeakerFrontLeft, SPA_AUDIO_CHANNEL_FL},    {kSpeakerFrontRight, SPA_AUDIO_CHANNEL_FR},
    {kSpeakerFrontCenter, SPA_AUDIO_CHANNEL_FC},  {kSpeakerLowFrequency, SPA_AUDIO_CHANNEL_LFE},
    {kSpeakerBackLeft, SPA_AUDIO_CHANNEL_RL},     {kSpeakerBackRight, SPA_AUDIO_CHANNEL_RR},
    {kSpeakerSideLeft, SPA_AUDIO_CHANNEL_SL},     {kSpeakerSideRight, SPA_AUDIO_CHANNEL_SR},
};

// False when the mask names a speaker the daemon never lays out.
bool positions_for(uint32_t channels, uint32_t speaker_mask, uint32_t* out) {
    // The daemon's one-channel sink is MONO, whatever bit the core calls it.
    if (channels == 1) {
        out[0] = SPA_AUDIO_CHANNEL_MONO;
        return true;
    }
    uint32_t n = 0;
    for (uint32_t bit = 1; bit != 0 && n < channels; bit <<= 1) {
        if ((speaker_mask & bit) == 0) continue;
        const auto* p = std::find_if(std::begin(kPositions), std::end(kPositions),
                                     [bit](const PositionBit& pb) { return pb.bit == bit; });
        if (p == std::end(kPositions)) return false;
        out[n++] = p->position;
    }
    return n == channels;
}

struct Stream {
    TestTone::Source* source = nullptr;
    std::atomic<uint64_t>* frames = nullptr;
    uint32_t channels = 0;
    uint32_t channel_mask = 0;
    double rate = 48000.0;
    pw_stream* stream = nullptr;
    std::vector<float> mono;
    bool failed = false;
    std::string error;
};

void on_process(void* data) {
    auto* s = static_cast<Stream*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
    if (b == nullptr) return;
    spa_data& d = b->buffer->datas[0];
    if (d.data == nullptr) {
        pw_stream_queue_buffer(s->stream, b);
        return;
    }
    const uint32_t stride = s->channels * sizeof(float);
    uint32_t n = d.maxsize / stride;
    if (b->requested != 0) n = std::min<uint32_t>(n, static_cast<uint32_t>(b->requested));
    n = std::min<uint32_t>(n, static_cast<uint32_t>(s->mono.size()));

    auto* out = static_cast<float*>(d.data);
    std::memset(out, 0, static_cast<size_t>(n) * stride);
    (*s->source)(s->mono.data(), n, s->rate);
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t c = 0; c < s->channels && c < 32; ++c) {
            if (s->channel_mask & (uint32_t{1} << c)) out[static_cast<size_t>(i) * s->channels + c] = s->mono[i];
        }
    }
    d.chunk->offset = 0;
    d.chunk->stride = static_cast<int32_t>(stride);
    d.chunk->size = n * stride;
    pw_stream_queue_buffer(s->stream, b);
    *s->frames += n;
}

void on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* error) {
    auto* s = static_cast<Stream*>(data);
    if (state != PW_STREAM_STATE_ERROR) return;
    s->failed = true;
    s->error = error != nullptr ? error : "";
}

// Filled field by field: a designated initializer would have to name every
// member to satisfy -Wmissing-field-initializers.
pw_stream_events stream_events() {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = on_state_changed;
    e.process = on_process;
    return e;
}

const pw_stream_events kStreamEvents = stream_events();

}  // namespace

TestTone::~TestTone() { stop(); }

void TestTone::start(const std::string& output, uint32_t channel_mask, Source source, Failure on_failure) {
    stop();
    stop_ = false;
    frames_ = 0;
    thread_ = std::thread([this, output, channel_mask, source = std::move(source), on_failure = std::move(on_failure)] {
        run(output, channel_mask, source, on_failure);
    });
}

void TestTone::start(const std::string& output, uint32_t channel, Failure on_failure) {
    // Built at the stream's rate, on the tone's thread.
    auto noise = std::make_shared<std::optional<PinkNoise>>();
    const Source source = [noise](float* out, uint32_t frames, double sample_rate) {
        if (!*noise) noise->emplace(sample_rate);
        for (uint32_t i = 0; i < frames; ++i) out[i] = (*noise)->next();
    };
    start(output, channel < 32 ? uint32_t{1} << channel : 0, source, std::move(on_failure));
}

void TestTone::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
}

void TestTone::run(std::string output, uint32_t channel_mask, Source source, Failure on_failure) {
    const auto fail = [&](int32_t code, const char* what) {
        if (on_failure) on_failure(code, what);
    };

    // The daemon's region for this output exists only while it feeds it, and
    // its header says what the core runs at.
    posix::SharedRegion region;
    if (const int error = region.open(posix::region_name(output)); error != 0) return fail(error, "region");
    const uint32_t channels = region.params()->hdr.channels;
    const uint32_t speaker_mask = region.params()->hdr.speaker_mask;
    // The daemon publishes the rate with its first processed block, which a
    // suspended sink has not had yet: the tone then goes at 48 kHz and PipeWire
    // converts it to whatever the graph runs at.
    const uint32_t rate = region.params()->hdr.sample_rate != 0 ? region.params()->hdr.sample_rate : 48000;
    region.close();
    if (channels == 0 || channels > kMaxChannels) return fail(ENODATA, "format");
    if (channel_mask == 0 || (channel_mask >> channels) != 0) return fail(EINVAL, "channel");
    uint32_t positions[kMaxChannels] = {};
    if (!positions_for(channels, speaker_mask, positions)) return fail(EINVAL, "layout");

    pw_init(nullptr, nullptr);
    pw_main_loop* loop = pw_main_loop_new(nullptr);
    if (loop == nullptr) {
        pw_deinit();
        return fail(errno != 0 ? errno : ENOMEM, "pw_main_loop_new");
    }

    Stream s;
    s.source = &source;
    s.frames = &frames_;
    s.channels = channels;
    s.channel_mask = channel_mask;
    s.rate = rate;
    s.mono.resize(8192);
    // A source that builds itself (the noise) does it before the stream runs.
    source(s.mono.data(), 0, s.rate);

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_MEDIA_ROLE, "Test", PW_KEY_NODE_NAME, "isotone-tone",
                                             PW_KEY_TARGET_OBJECT, kIsotoneSinkName,
                                             // Never somewhere else: a tone that cannot reach the
                                             // core must not play unprocessed.
                                             PW_KEY_NODE_DONT_RECONNECT, "true", nullptr);
    s.stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), "Isotone tone", props, &kStreamEvents, &s);
    if (s.stream == nullptr) {
        const int error = errno != 0 ? errno : ENOMEM;
        pw_main_loop_destroy(loop);
        pw_deinit();
        return fail(error, "pw_stream_new_simple");
    }

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = rate;
    info.channels = channels;
    std::copy(positions, positions + channels, info.position);
    const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    const int connected = pw_stream_connect(
        s.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        // Not RT_PROCESS: the source is called on this thread, as it is on Windows.
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
    if (connected < 0) {
        pw_stream_destroy(s.stream);
        pw_main_loop_destroy(loop);
        pw_deinit();
        return fail(-connected, "pw_stream_connect");
    }

    // The loop runs until stop() or a failed stream; a timer looks every 10 ms,
    // since stop() is called from another thread.
    struct Watch {
        pw_main_loop* loop;
        const std::atomic<bool>* stop;
        const Stream* stream;
    } watch{loop, &stop_, &s};
    spa_source* timer = pw_loop_add_timer(
        pw_main_loop_get_loop(loop),
        [](void* data, uint64_t) {
            auto* w = static_cast<Watch*>(data);
            if (*w->stop || w->stream->failed) pw_main_loop_quit(w->loop);
        },
        &watch);
    timespec interval{0, 10 * 1000 * 1000};
    pw_loop_update_timer(pw_main_loop_get_loop(loop), timer, &interval, &interval, false);
    pw_main_loop_run(loop);
    pw_loop_destroy_source(pw_main_loop_get_loop(loop), timer);

    pw_stream_destroy(s.stream);
    pw_main_loop_destroy(loop);
    pw_deinit();
    if (s.failed) fail(EIO, "stream");
}

}  // namespace isotone::ui
