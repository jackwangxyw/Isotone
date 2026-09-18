// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Stage 1c spike: isotone_core's Processor hosted in a PipeWire filter node,
// to decide the Linux topology by measurement the way stages 1a and 1b were
// decided on Windows (decisions.md, "Stage 1b, IsoAPO built and verified
// offline"). It is not the daemon: no shared-memory transport, no saved state,
// no default-sink tracking, no rate renegotiation off the real-time thread.
//
// The node carries two DSP input ports and two DSP output ports, so one graph
// cycle hands us both sides and there is no ring buffer or second clock between
// capture and playback. The links are made from outside with pw-link, so the
// topology under test is visible in the measurement script rather than buried
// here.
//
// --bypass runs the same graph with an empty EqState, which is the baseline the
// filtered run is measured against.

#include <pipewire/pipewire.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "isotone/processor.h"
#include "isotone/types.h"

namespace {

constexpr uint32_t kChannels = 2;

struct Spike {
    pw_main_loop* loop   = nullptr;
    pw_filter*    filter = nullptr;
    void*         in_port[kChannels]  = {};
    void*         out_port[kChannels] = {};

    isotone::Processor proc;
    isotone::EqState   state;
    double             rate       = 0.0;
    uint32_t           max_frames = 0;
};

// Called on the real-time thread (PW_FILTER_FLAG_RT_PROCESS).
void on_process(void* userdata, spa_io_position* position) {
    auto* s = static_cast<Spike*>(userdata);

    const uint32_t frames = position->clock.duration;
    const double   rate   = position->clock.rate.denom != 0
                                ? static_cast<double>(position->clock.rate.denom)
                                : 48000.0;

    float* in[kChannels]  = {};
    float* out[kChannels] = {};
    for (uint32_t c = 0; c < kChannels; ++c) {
        in[c]  = static_cast<float*>(pw_filter_get_dsp_buffer(s->in_port[c], frames));
        out[c] = static_cast<float*>(pw_filter_get_dsp_buffer(s->out_port[c], frames));
    }

    for (uint32_t c = 0; c < kChannels; ++c) {
        if (out[c] == nullptr) continue;
        if (in[c] == nullptr) {
            std::memset(out[c], 0, frames * sizeof(float));
        } else if (in[c] != out[c]) {
            std::memcpy(out[c], in[c], frames * sizeof(float));
        }
    }

    // The one thing the daemon must do differently: initialize() allocates and
    // is not real-time safe. Here the graph is fixed for the length of a
    // measurement, so it runs at most once. The daemon has to react to a rate
    // or quantum change on its main loop and hand the processor over.
    if (s->rate != rate || s->max_frames < frames) {
        s->proc.initialize(rate, kChannels, frames);
        s->proc.set_target(s->state);
        s->proc.reset();
        s->rate       = rate;
        s->max_frames = frames;
    }

    bool have_all = true;
    for (uint32_t c = 0; c < kChannels; ++c)
        if (out[c] == nullptr) have_all = false;
    if (!have_all) return;

    s->proc.process(out, frames);
}

const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .destroy = nullptr,
    .state_changed = nullptr,
    .io_changed = nullptr,
    .param_changed = nullptr,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_process,
    .drained = nullptr,
    .command = nullptr,
};

void on_signal(void* userdata, int /*signal_number*/) {
    auto* s = static_cast<Spike*>(userdata);
    pw_main_loop_quit(s->loop);
}

void* add_dsp_port(pw_filter* filter, pw_direction dir, const char* name) {
    return pw_filter_add_port(
        filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, name, nullptr),
        nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
    bool bypass = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--bypass") bypass = true;

    isotone::enable_denormal_flushing();

    Spike spike;
    if (!bypass) {
        // The same filter stage 1b measured on Windows: peaking, 1 kHz, -12 dB,
        // Q 1. Analytic response -12.000 dB at 1 kHz and -0.161 dB at 100 Hz.
        isotone::Band band;
        band.id         = 1;
        band.type       = isotone::FilterType::Peaking;
        band.fc         = 1000.0;
        band.gain_db    = -12.0;
        band.width      = 1.0;
        band.width_mode = isotone::WidthMode::Q;
        spike.state.bands.push_back(band);
    }

    pw_init(&argc, &argv);

    spike.loop = pw_main_loop_new(nullptr);
    if (spike.loop == nullptr) {
        std::fprintf(stderr, "pw_main_loop_new failed\n");
        return 1;
    }
    pw_loop_add_signal(pw_main_loop_get_loop(spike.loop), SIGINT, on_signal, &spike);
    pw_loop_add_signal(pw_main_loop_get_loop(spike.loop), SIGTERM, on_signal, &spike);

    spike.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(spike.loop), "isotone-spike",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Filter",
                          PW_KEY_MEDIA_ROLE, "DSP",
                          PW_KEY_NODE_NAME, "isotone-spike",
                          PW_KEY_NODE_DESCRIPTION, "Isotone stage 1c spike",
                          PW_KEY_NODE_AUTOCONNECT, "false", nullptr),
        &kFilterEvents, &spike);
    if (spike.filter == nullptr) {
        std::fprintf(stderr, "pw_filter_new_simple failed\n");
        return 1;
    }

    static const char* kInNames[kChannels]  = {"in_FL", "in_FR"};
    static const char* kOutNames[kChannels] = {"out_FL", "out_FR"};
    for (uint32_t c = 0; c < kChannels; ++c) {
        spike.in_port[c]  = add_dsp_port(spike.filter, PW_DIRECTION_INPUT, kInNames[c]);
        spike.out_port[c] = add_dsp_port(spike.filter, PW_DIRECTION_OUTPUT, kOutNames[c]);
        if (spike.in_port[c] == nullptr || spike.out_port[c] == nullptr) {
            std::fprintf(stderr, "pw_filter_add_port failed\n");
            return 1;
        }
    }

    if (pw_filter_connect(spike.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::fprintf(stderr, "pw_filter_connect failed\n");
        return 1;
    }

    std::fprintf(stderr, "isotone-spike: %s\n", bypass ? "bypass (baseline)" : "peaking 1 kHz -12 dB Q 1");
    std::fflush(stderr);

    pw_main_loop_run(spike.loop);

    pw_filter_destroy(spike.filter);
    pw_main_loop_destroy(spike.loop);
    pw_deinit();
    return 0;
}
