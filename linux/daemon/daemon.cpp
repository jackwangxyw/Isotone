// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "daemon.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/json.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"
#include "isotone/types.h"
#include "persisted_state.h"
#include "shared_region.h"

namespace isotone::daemon {

namespace {

// The layouts a virtual sink can carry, named the way PipeWire names channels
// and mapped to the speaker bits the core addresses bands and routing by
// (isotone/speakers.h). The port names follow: a null sink with
// audio.position [ FL FR ] carries monitor_FL and playback_FL.
struct Position {
    const char* name;
    uint32_t    speaker;
};

struct Layout {
    uint32_t       channels;
    const Position positions[kMaxChannels];
};

constexpr Layout kLayouts[] = {
    {1, {{"MONO", kSpeakerFrontCenter}}},
    {2, {{"FL", kSpeakerFrontLeft}, {"FR", kSpeakerFrontRight}}},
    {3, {{"FL", kSpeakerFrontLeft}, {"FR", kSpeakerFrontRight}, {"LFE", kSpeakerLowFrequency}}},
    {4, {{"FL", kSpeakerFrontLeft}, {"FR", kSpeakerFrontRight},
         {"RL", kSpeakerBackLeft}, {"RR", kSpeakerBackRight}}},
    {6, {{"FL", kSpeakerFrontLeft}, {"FR", kSpeakerFrontRight},
         {"FC", kSpeakerFrontCenter}, {"LFE", kSpeakerLowFrequency},
         {"RL", kSpeakerBackLeft}, {"RR", kSpeakerBackRight}}},
    {8, {{"FL", kSpeakerFrontLeft}, {"FR", kSpeakerFrontRight},
         {"FC", kSpeakerFrontCenter}, {"LFE", kSpeakerLowFrequency},
         {"RL", kSpeakerBackLeft}, {"RR", kSpeakerBackRight},
         {"SL", kSpeakerSideLeft}, {"SR", kSpeakerSideRight}}},
};

const Layout* layout_for(uint32_t channels) {
    for (const Layout& l : kLayouts)
        if (l.channels == channels) return &l;
    return nullptr;
}

struct NodeInfo {
    std::string name;
    std::string media_class;
    bool        capture = false;   // an application's playback (capture_streams)
};

struct PortInfo {
    uint32_t    node_id = 0;
    std::string name;
    bool        is_output = false;
};

// What on_process() asks the main loop for when it cannot run at the graph's
// current shape. initialize() allocates, so it never runs on the audio thread.
struct InitRequest {
    uint32_t rate;
    uint32_t frames;
};

struct Daemon {
    Options options;

    pw_main_loop* loop     = nullptr;
    pw_context*   context  = nullptr;
    pw_core*      core     = nullptr;
    pw_registry*  registry = nullptr;
    spa_hook      registry_hook{};

    pw_metadata* metadata = nullptr;
    uint32_t     metadata_id = SPA_ID_INVALID;
    spa_hook     metadata_hook{};

    pw_proxy*              virtual_sink = nullptr;
    std::vector<pw_proxy*> input_links;    // our sink's monitor into the core
    std::vector<pw_proxy*> output_links;   // the core into the sink being fed
    // Per channel, so a link the registry could not satisfy yet is made when its
    // port arrives instead of leaving that channel silent for the run, and so a
    // retry never links a channel twice.
    std::vector<bool> input_linked;
    std::vector<bool> output_linked;

    pw_filter* filter = nullptr;
    void*      in_port[kMaxChannels]  = {};
    void*      out_port[kMaxChannels] = {};

    // Fixed once, before the filter connects, so the audio thread may read them.
    uint32_t      channels = 2;
    uint32_t      speaker_mask = 0;
    const Layout* layout = nullptr;

    std::map<uint32_t, NodeInfo> nodes;
    std::map<uint32_t, PortInfo> ports;

    // Playback streams given a target.object (capture_streams), so exit can
    // clear exactly those.
    std::vector<uint32_t> moved_streams;

    // The sink being fed. With --sink this never changes; without it, it follows
    // the default sink.
    std::string target;

    posix::SharedRegion region;
    std::string         region_name;

    Processor  processor;
    EqState    state;
    ParamBlock block{};
    uint32_t   applied_seq = 0;

    // The post-EQ ring the UI's spectrum drains. The writer takes interleaved
    // frames and the filter hands us planar ones, so the audio thread
    // interleaves into this buffer, sized on the main loop with everything else.
    AudioRingWriter    ring;
    std::vector<float> interleaved;
    uint64_t           ring_token = 0;

    // Read on the audio thread, written on the main loop.
    std::atomic<bool> ready{false};
    uint32_t          proc_rate = 0;
    uint32_t          proc_max_frames = 0;
    std::atomic<bool> init_pending{false};
    // Held across the body of on_process, so the main loop can tell when the
    // audio thread is no longer reading the region it is about to unmap.
    std::atomic<bool> in_process{false};

    std::atomic<uint64_t> blocks_processed{0};
};

// ---------------------------------------------------------------- processing

// Main loop. Sizing the processor allocates, so it happens nowhere else.
int do_initialize(spa_loop*, bool, uint32_t, const void* data, size_t, void* user_data) {
    auto* d = static_cast<Daemon*>(user_data);
    const auto* request = static_cast<const InitRequest*>(data);

    const uint32_t frames =
        request->frames > d->options.max_frames ? request->frames : d->options.max_frames;
    d->processor.initialize(request->rate, d->channels, frames);

    // from_param_block reuses this capacity, so applying a block on the audio
    // thread never allocates.
    d->state.bands.reserve(kParamMaxBands);

    ParamBlock* shared = d->region.params();
    if (shared != nullptr && param_block_read(shared, &d->block, 1000)) {
        d->applied_seq = d->block.hdr.seq;
        from_param_block(d->block, &d->state);
        remap_channels(&d->state, ChannelLayout{d->channels, d->speaker_mask});
    }
    d->processor.set_target(d->state);
    d->processor.reset();

    d->proc_rate = request->rate;
    d->proc_max_frames = frames;

    // Interleaving happens on the audio thread, so its buffer is sized here.
    d->interleaved.assign(static_cast<size_t>(frames) * d->channels, 0.0f);
    if (shared != nullptr) {
        d->ring.set_channels(d->channels);
        // Here rather than on the audio thread, whose first lap through the ring
        // would otherwise fault every page in.
        d->ring.prefault();
        d->ring.claim(d->ring_token);
        // Published whether or not the ring was won: the rate, the channel count
        // and the host's state are the format, and a UI needs them even while
        // another instance still owns the ring.
        host_publish_format(shared, request->rate, d->channels, d->speaker_mask,
                            HostState::Running);
    }
    d->ready.store(true, std::memory_order_release);
    d->init_pending.store(false, std::memory_order_release);
    return 0;
}

// Audio thread (PW_FILTER_FLAG_RT_PROCESS).
// Sets a flag for the whole of on_process. quiesce() clears `ready` and then
// waits for this, which is what makes unmapping the region safe.
struct InProcessGuard {
    std::atomic<bool>& flag;
    explicit InProcessGuard(std::atomic<bool>& f) : flag(f) { flag.store(true); }
    ~InProcessGuard() { flag.store(false); }
    InProcessGuard(const InProcessGuard&) = delete;
    InProcessGuard& operator=(const InProcessGuard&) = delete;
};

void on_process(void* userdata, spa_io_position* position) {
    auto* d = static_cast<Daemon*>(userdata);
    const InProcessGuard guard(d->in_process);

    const uint32_t frames = position->clock.duration;
    const uint32_t rate   = position->clock.rate.denom != 0 ? position->clock.rate.denom : 48000;

    float* in[kMaxChannels]  = {};
    float* out[kMaxChannels] = {};
    for (uint32_t c = 0; c < d->channels; ++c) {
        in[c]  = static_cast<float*>(pw_filter_get_dsp_buffer(d->in_port[c], frames));
        out[c] = static_cast<float*>(pw_filter_get_dsp_buffer(d->out_port[c], frames));
    }
    for (uint32_t c = 0; c < d->channels; ++c) {
        if (out[c] == nullptr) return;
        if (in[c] == nullptr) {
            std::memset(out[c], 0, frames * sizeof(float));
        } else if (in[c] != out[c]) {
            std::memcpy(out[c], in[c], frames * sizeof(float));
        }
    }

    // Sequentially consistent against quiesce(): either this call sees `ready`
    // already false and touches no region, or quiesce sees in_process and waits.
    const bool fits = d->ready.load() && rate == d->proc_rate && frames <= d->proc_max_frames;
    if (!fits) {
        // Pass the audio through untouched and ask the main loop to resize. One
        // request at a time, or a stalled main loop would queue thousands.
        bool expected = false;
        if (d->init_pending.compare_exchange_strong(expected, true)) {
            d->ready.store(false);
            const InitRequest request{rate, frames};
            const int queued = pw_loop_invoke(pw_main_loop_get_loop(d->loop), do_initialize, 0,
                                              &request, sizeof(request), false, d);
            // A refused invoke would otherwise leave init_pending set for good,
            // and every later cycle would fail the exchange and pass the audio
            // through untouched with nothing asking for a resize again.
            if (queued < 0) d->init_pending.store(false);
        }
        return;
    }

    ParamBlock* shared = d->region.params();
    if (shared != nullptr) {
        // A ring another instance still holds is taken over only after its write
        // index has stood still for a while, so the claim has to be retried from
        // here rather than made once at startup; audio_ring.h says as much, and
        // IsoApo::APOProcess does the same. A daemon that was killed leaves its
        // claim behind, and without this the spectrum would be dead for the whole
        // of the next run, and the format never published.
        if (!d->ring.owns() && d->ring.claim(d->ring_token) && d->ring.owns()) {
            host_publish_format(shared, d->proc_rate, d->channels, d->speaker_mask,
                                HostState::Running);
        }
        host_heartbeat(shared);
        // Copy only when something has been written since the last apply. A
        // failed read keeps the current parameters and retries next cycle.
        if (param_block_seq(shared) != d->applied_seq && param_block_read(shared, &d->block)) {
            d->applied_seq = d->block.hdr.seq;
            from_param_block(d->block, &d->state);
            remap_channels(&d->state, ChannelLayout{d->channels, d->speaker_mask});
            d->processor.set_target(d->state);
        }
    }

    d->processor.process(out, frames);

    // The spectrum's audio. Only the owner writes; another process taking the
    // claim ends this one's, and the next block notices.
    if (shared != nullptr && static_cast<size_t>(frames) * d->channels <= d->interleaved.size()) {
        float* dst = d->interleaved.data();
        for (uint32_t c = 0; c < d->channels; ++c)
            for (uint32_t i = 0; i < frames; ++i) dst[i * d->channels + c] = out[c][i];
        d->ring.write(dst, d->channels, frames);
    }
    d->blocks_processed.fetch_add(1, std::memory_order_relaxed);

    if (d->options.exit_when_linked && d->blocks_processed.load(std::memory_order_relaxed) > 200) {
        pw_main_loop_quit(d->loop);
    }
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

// ------------------------------------------------------------------ linking

bool find_port(const Daemon& d, const std::string& node_name, const std::string& port_name,
               bool is_output, uint32_t* id_out) {
    for (const auto& [id, port] : d.ports) {
        if (port.is_output != is_output || port.name != port_name) continue;
        const auto node = d.nodes.find(port.node_id);
        if (node == d.nodes.end() || node->second.name != node_name) continue;
        *id_out = id;
        return true;
    }
    return false;
}

bool find_port_by_node_id(const Daemon& d, uint32_t node_id, const std::string& port_name,
                          bool is_output, uint32_t* id_out) {
    for (const auto& [id, port] : d.ports) {
        if (port.node_id != node_id || port.is_output != is_output || port.name != port_name) continue;
        *id_out = id;
        return true;
    }
    return false;
}

pw_proxy* make_link(Daemon& d, uint32_t output_port, uint32_t input_port) {
    pw_properties* props = pw_properties_new(nullptr, nullptr);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output_port);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input_port);
    pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");

    auto* link = static_cast<pw_proxy*>(pw_core_create_object(
        d.core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
    pw_properties_free(props);
    return link;
}

std::string port_name(const char* prefix, const Position& position) {
    return std::string(prefix) + position.name;
}

// Our own sink's monitor into the core. Done once: the virtual sink is ours and
// does not change.
// Channel by channel, and only the ones not linked yet. Linking the whole set or
// nothing meant that a failure part way through left links behind while the
// "done" flag stayed clear, so the next registry event linked those channels a
// second time and they came out about 6 dB hot.
void link_input(Daemon& d) {
    if (d.filter == nullptr) return;
    const uint32_t filter_node = pw_filter_get_node_id(d.filter);
    if (filter_node == SPA_ID_INVALID) return;

    for (uint32_t c = 0; c < d.channels; ++c) {
        if (d.input_linked[c]) continue;
        const Position& p = d.layout->positions[c];
        uint32_t monitor = 0, filter_in = 0;
        if (!find_port(d, d.options.sink_name, port_name("monitor_", p), true, &monitor)) continue;
        if (!find_port_by_node_id(d, filter_node, port_name("in_", p), false, &filter_in)) continue;
        pw_proxy* link = make_link(d, monitor, filter_in);
        if (link == nullptr) {
            std::fprintf(stderr, "isotone-daemon: could not link %s into the core\n", p.name);
            continue;
        }
        d.input_links.push_back(link);
        d.input_linked[c] = true;
    }
}

void unlink_output(Daemon& d) {
    for (pw_proxy* link : d.output_links) pw_proxy_destroy(link);
    d.output_links.clear();
    d.output_linked.assign(d.channels, false);
}

// The core into the sink being fed, one channel at a time and only those not
// linked yet, so a port that had not reached the registry when the target
// changed is picked up when it arrives instead of leaving that channel silent
// for the run. A target that genuinely lacks a position simply never gets it.
bool link_output(Daemon& d) {
    if (d.filter == nullptr || d.target.empty()) return false;
    const uint32_t filter_node = pw_filter_get_node_id(d.filter);
    if (filter_node == SPA_ID_INVALID) return false;

    uint32_t made = 0;
    for (uint32_t c = 0; c < d.channels; ++c) {
        if (d.output_linked[c]) continue;
        const Position& p = d.layout->positions[c];
        uint32_t filter_out = 0, playback = 0;
        if (!find_port_by_node_id(d, filter_node, port_name("out_", p), true, &filter_out)) continue;
        if (!find_port(d, d.target, port_name("playback_", p), false, &playback)) continue;
        pw_proxy* link = make_link(d, filter_out, playback);
        if (link == nullptr) {
            std::fprintf(stderr, "isotone-daemon: could not link %s into %s\n", p.name,
                         d.target.c_str());
            continue;
        }
        d.output_links.push_back(link);
        d.output_linked[c] = true;
        ++made;
    }
    if (made > 0) {
        uint32_t total = 0;
        for (uint32_t c = 0; c < d.channels; ++c) total += d.output_linked[c] ? 1u : 0u;
        std::fprintf(stderr, "isotone-daemon: %s -> core -> %s (%u of %u channels)\n",
                     d.options.sink_name.c_str(), d.target.c_str(), total, d.channels);
        std::fflush(stderr);
    }
    return made > 0;
}

void try_link(Daemon& d) {
    link_input(d);
    link_output(d);
}

// -------------------------------------------------------- region and target

// Fills a region this daemon creates with the sink's saved state, before any
// other process can see the region as valid. Flat when nothing is saved.
void seed_from_saved(ParamBlock* block, void* context) {
    const auto* saved = static_cast<const ParamBlock*>(context);
    if (saved == nullptr) return;
    const ParamBlockHeader header = block->hdr;
    *block = *saved;
    block->hdr = header;   // the live region's header is the host's, not the file's
}

// Waits for the audio thread to leave on_process, so what it reads can be
// unmapped from under it. `ready` is cleared first and the two flags are
// sequentially consistent, so a call starting after this returns before it
// touches the region; one already past that point is waited for.
void quiesce(Daemon& d) {
    d.ready.store(false);
    for (int i = 0; i < 400 && d.in_process.load(); ++i) {
        timespec nap{0, 500000};   // 0.5 ms, so at most 200
        ::nanosleep(&nap, nullptr);
    }
}

void close_region(Daemon& d) {
    if (!d.region.is_open()) return;
    quiesce(d);
    if (ParamBlock* shared = d.region.params(); shared != nullptr) {
        host_publish_format(shared, d.proc_rate, d.channels, d.speaker_mask,
                            HostState::NotLoaded);
    }
    d.ring.release();
    d.region.close();
    if (d.options.unlink_on_exit) posix::SharedRegion::unlink_region(d.region_name);
    d.region_name.clear();
}

bool open_region_for(Daemon& d, const std::string& sink) {
    d.region_name = posix::region_name(sink);
    if (d.region_name.empty()) return false;

    const std::string dir =
        d.options.state_dir.empty() ? posix::persisted_state_dir() : d.options.state_dir;
    ParamBlock saved{};
    bool have_saved = false;
    if (!dir.empty()) {
        const std::string path = posix::persisted_state_path(dir, sink);
        have_saved = posix::read_persisted_state(path, &saved) == posix::PersistedRead::Loaded;
    }

    const int error = d.region.create_or_open(d.region_name, have_saved ? seed_from_saved : nullptr,
                                              have_saved ? &saved : nullptr);
    if (error != 0) {
        std::fprintf(stderr, "isotone-daemon: shared region %s: %s\n", d.region_name.c_str(),
                     std::strerror(error));
        return false;
    }
    std::fprintf(stderr, "isotone-daemon: region %s (%s, saved state %s)\n", d.region_name.c_str(),
                 d.region.created() ? "created" : "adopted", have_saved ? "loaded" : "none");

    d.ring.attach(d.region.ring(), kRingCapacityFrames);
    // The layout is fixed for the run, so the UI learns it now rather than at the
    // first processed block, which a suspended sink may not see for a long time.
    // The rate is the graph's and comes with that block.
    host_publish_format(d.region.params(), 0, d.channels, d.speaker_mask, HostState::NotLoaded);
    return true;
}

// Moves to another sink: its links, its region, its saved state. The processor
// is re-sized rather than reused, because the new sink may run at another rate.
// That is the main loop's job, so `ready` is simply dropped and the next audio
// block asks for it.
void set_target(Daemon& d, const std::string& sink) {
    if (sink.empty() || sink == d.target) return;
    // Feeding our own virtual sink would be a loop: its monitor is the core's
    // input. This is the ordinary state once Isotone is the default sink, so it
    // is not an error, and the sink we were already feeding stays.
    if (sink == d.options.sink_name) return;

    std::fprintf(stderr, "isotone-daemon: target %s -> %s\n",
                 d.target.empty() ? "(none)" : d.target.c_str(), sink.c_str());
    unlink_output(d);
    close_region(d);
    d.ready.store(false, std::memory_order_release);
    d.target = sink;
    if (!open_region_for(d, sink)) {
        d.target.clear();
        return;
    }
    link_output(d);
}

// ----------------------------------------------------------------- metadata

// default.audio.sink carries {"name":"<node.name>"}.
std::string default_sink_name(const char* value) {
    if (value == nullptr) return {};
    spa_json object;
    spa_json_init(&object, value, std::strlen(value));
    spa_json fields;
    if (spa_json_enter_object(&object, &fields) <= 0) return {};

    char key[128];
    while (spa_json_get_string(&fields, key, sizeof(key)) > 0) {
        if (std::strcmp(key, "name") == 0) {
            char name[256];
            return spa_json_get_string(&fields, name, sizeof(name)) > 0 ? std::string(name)
                                                                       : std::string();
        }
        const char* skip = nullptr;
        if (spa_json_next(&fields, &skip) <= 0) break;
    }
    return {};
}

int on_metadata_property(void* data, uint32_t subject, const char* key, const char* /*type*/,
                         const char* value) {
    auto* d = static_cast<Daemon*>(data);
    if (key == nullptr) return 0;
    // A stream someone has since moved elsewhere is theirs: exit must not undo it.
    if (subject != PW_ID_CORE && std::strcmp(key, "target.object") == 0 &&
        (value == nullptr || d->options.sink_name != value)) {
        std::erase(d->moved_streams, subject);
        return 0;
    }
    if (subject != PW_ID_CORE) return 0;
    if (std::strcmp(key, "default.audio.sink") != 0) return 0;
    // Only when following: with --sink the target is the owner's choice and the
    // session manager does not get a vote.
    if (!d->options.target_sink.empty()) return 0;
    set_target(*d, default_sink_name(value));
    return 0;
}

// ------------------------------------------------------------------ streams

// A stream the desktop would play straight to the default sink: an
// application's playback, not one that named its own target (pw-play --target,
// Isotone's own tones, the measurement rig) and not one of ours.
bool should_capture(const spa_dict* props) {
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class == nullptr || std::strcmp(media_class, "Stream/Output/Audio") != 0) return false;
    if (spa_dict_lookup(props, PW_KEY_TARGET_OBJECT) != nullptr) return false;
    if (spa_dict_lookup(props, PW_KEY_NODE_TARGET) != nullptr) return false;
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    return name == nullptr || std::strncmp(name, "isotone", 7) != 0;
}

// WirePlumber reads target.object from the default metadata and, finding no
// number there, matches it against node.name.
void capture_stream(Daemon& d, uint32_t id) {
    if (d.metadata == nullptr) return;   // done for every stream once it is bound
    pw_metadata_set_property(d.metadata, id, "target.object", "Spa:String", d.options.sink_name.c_str());
    d.moved_streams.push_back(id);
}



const pw_metadata_events kMetadataEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

// ----------------------------------------------------------------- registry

void on_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
               uint32_t /*version*/, const spa_dict* props) {
    auto* d = static_cast<Daemon*>(data);
    if (props == nullptr) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        NodeInfo info;
        if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) info.name = name;
        if (const char* mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) info.media_class = mc;
        info.capture = d->options.capture_streams && should_capture(props);
        d->nodes[id] = info;
        if (info.capture) capture_stream(*d, id);
        try_link(*d);
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char* node_id = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char* name    = spa_dict_lookup(props, PW_KEY_PORT_NAME);
        const char* dir     = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (node_id == nullptr || name == nullptr || dir == nullptr) return;
        PortInfo port;
        port.node_id   = static_cast<uint32_t>(std::strtoul(node_id, nullptr, 10));
        port.name      = name;
        port.is_output = std::strcmp(dir, "out") == 0;
        d->ports[id] = port;
        try_link(*d);
        return;
    }

    // Following the default reads it; capturing streams writes it.
    if ((d->options.target_sink.empty() || d->options.capture_streams) && d->metadata == nullptr &&
        std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, "metadata.name");
        if (name == nullptr || std::strcmp(name, "default") != 0) return;
        d->metadata =
            static_cast<pw_metadata*>(pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0));
        if (d->metadata != nullptr) {
            d->metadata_id = id;
            pw_metadata_add_listener(d->metadata, &d->metadata_hook, &kMetadataEvents, d);
            // Streams that arrived first, or all of them again after a
            // WirePlumber restart, which forgets every target it held.
            d->moved_streams.clear();
            for (const auto& [node_id, node] : d->nodes)
                if (node.capture) capture_stream(*d, node_id);
        }
    }
}

void on_global_remove(void* data, uint32_t id) {
    auto* d = static_cast<Daemon*>(data);

    // WirePlumber restarting takes the default metadata with it. Forgetting the
    // proxy is what lets the replacement be bound: keeping it left a zombie that
    // emitted nothing and a guard that refused to bind the new one, so the
    // daemon silently stopped following the default from then on.
    if (d->metadata != nullptr && d->metadata_id == id) {
        spa_hook_remove(&d->metadata_hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d->metadata));
        d->metadata = nullptr;
        d->metadata_id = SPA_ID_INVALID;
    }

    const auto node = d->nodes.find(id);
    if (node != d->nodes.end() && !d->target.empty() && node->second.name == d->target) {
        // The server has already destroyed the links; the client-side proxies
        // still have to go, or every replug leaks one per channel.
        unlink_output(*d);
        // Only when following. With --sink the name is the owner's choice and
        // the same device coming back must be picked up again; clearing it left
        // the daemon permanently silent after one unplug, because nothing but
        // the metadata listener ever sets a target and that is not bound.
        if (d->options.target_sink.empty()) d->target.clear();
    }
    d->nodes.erase(id);
    d->ports.erase(id);
    std::erase(d->moved_streams, id);
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

// Runs the loop until the server has answered everything sent so far, or a
// second has passed (a server that is itself going away answers nothing).
void roundtrip(Daemon& d) {
    struct Wait {
        pw_main_loop* loop;
        int seq;
    } wait{d.loop, 0};
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = [](void* data, uint32_t id, int seq) {
        auto* w = static_cast<Wait*>(data);
        if (id == PW_ID_CORE && seq == w->seq) pw_main_loop_quit(w->loop);
    };
    spa_hook hook{};
    pw_core_add_listener(d.core, &hook, &events, &wait);
    wait.seq = pw_core_sync(d.core, PW_ID_CORE, 0);

    pw_loop* loop = pw_main_loop_get_loop(d.loop);
    spa_source* timeout = pw_loop_add_timer(
        loop, [](void* data, uint64_t) { pw_main_loop_quit(static_cast<pw_main_loop*>(data)); }, d.loop);
    timespec second{1, 0};
    pw_loop_update_timer(loop, timeout, &second, nullptr, false);
    pw_main_loop_run(d.loop);
    pw_loop_destroy_source(loop, timeout);
    spa_hook_remove(&hook);
}

// The targets this run set go, so the streams return to the default sink rather
// than following a sink that is about to disappear.
void release_streams(Daemon& d) {
    if (d.metadata == nullptr || d.moved_streams.empty()) return;
    for (uint32_t id : d.moved_streams) pw_metadata_set_property(d.metadata, id, "target.object", nullptr, nullptr);
    d.moved_streams.clear();
    roundtrip(d);
}

// -------------------------------------------------------------------- setup

void on_signal(void* userdata, int /*signal_number*/) {
    pw_main_loop_quit(static_cast<Daemon*>(userdata)->loop);
}

void* add_dsp_port(pw_filter* filter, pw_direction dir, const char* name) {
    return pw_filter_add_port(
        filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name,
                          nullptr),
        nullptr, 0);
}

bool create_virtual_sink(Daemon& d) {
    std::string position = "[";
    for (uint32_t c = 0; c < d.channels; ++c) {
        position += " ";
        position += d.layout->positions[c].name;
    }
    position += " ]";

    pw_properties* props = pw_properties_new(
        PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_NODE_NAME, d.options.sink_name.c_str(),
        PW_KEY_NODE_DESCRIPTION, d.options.sink_description.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
        "audio.position", position.c_str(), "monitor.channel-volumes", "false", PW_KEY_OBJECT_LINGER,
        "false", nullptr);

    d.virtual_sink = static_cast<pw_proxy*>(pw_core_create_object(
        d.core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
    pw_properties_free(props);

    if (d.virtual_sink == nullptr) {
        std::fprintf(stderr, "isotone-daemon: could not create the virtual sink\n");
        return false;
    }
    return true;
}

}  // namespace

int run(const Options& options) {
    enable_denormal_flushing();

    Daemon d;
    d.options = options;

    pw_init(nullptr, nullptr);

    d.loop = pw_main_loop_new(nullptr);
    if (d.loop == nullptr) {
        std::fprintf(stderr, "isotone-daemon: pw_main_loop_new failed\n");
        return 1;
    }
    pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGINT, on_signal, &d);
    pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGTERM, on_signal, &d);

    d.context = pw_context_new(pw_main_loop_get_loop(d.loop), nullptr, 0);
    d.core = d.context != nullptr ? pw_context_connect(d.context, nullptr, 0) : nullptr;
    if (d.core == nullptr) {
        std::fprintf(stderr, "isotone-daemon: no PipeWire daemon to connect to\n");
        return 1;
    }

    d.layout = layout_for(options.channels);
    if (d.layout == nullptr) {
        std::fprintf(stderr, "isotone-daemon: no layout for %u channels\n", options.channels);
        return 1;
    }
    d.channels = options.channels;
    for (uint32_t c = 0; c < d.channels; ++c) d.speaker_mask |= d.layout->positions[c].speaker;
    d.input_linked.assign(d.channels, false);
    d.output_linked.assign(d.channels, false);

    // Names this run, not this process: a pid is reused, and a stale claim left
    // by a killed daemon must not be mistaken for a live one.
    std::random_device entropy;
    d.ring_token = (static_cast<uint64_t>(entropy()) << 32) | entropy();

    if (!options.target_sink.empty()) {
        d.target = options.target_sink;
        if (!open_region_for(d, d.target)) return 1;
    } else {
        std::fprintf(stderr, "isotone-daemon: following the default sink\n");
    }
    if (!create_virtual_sink(d)) {
        close_region(d);
        return 1;
    }

    d.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(d.loop), "isotone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter",
                          PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_NODE_NAME, "isotone-core",
                          PW_KEY_NODE_DESCRIPTION, "Isotone", PW_KEY_NODE_AUTOCONNECT, "false",
                          nullptr),
        &kFilterEvents, &d);
    if (d.filter == nullptr) {
        std::fprintf(stderr, "isotone-daemon: pw_filter_new_simple failed\n");
        close_region(d);
        return 1;
    }

    for (uint32_t c = 0; c < d.channels; ++c) {
        const Position& p = d.layout->positions[c];
        d.in_port[c]  = add_dsp_port(d.filter, PW_DIRECTION_INPUT, port_name("in_", p).c_str());
        d.out_port[c] = add_dsp_port(d.filter, PW_DIRECTION_OUTPUT, port_name("out_", p).c_str());
        if (d.in_port[c] == nullptr || d.out_port[c] == nullptr) {
            std::fprintf(stderr, "isotone-daemon: pw_filter_add_port failed\n");
            close_region(d);
            return 1;
        }
    }
    if (pw_filter_connect(d.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::fprintf(stderr, "isotone-daemon: pw_filter_connect failed\n");
        close_region(d);
        return 1;
    }

    d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d.registry, &d.registry_hook, &kRegistryEvents, &d);

    pw_main_loop_run(d.loop);

    release_streams(d);

    // Teardown. The filter goes first: while it is connected the audio thread is
    // still being scheduled, and unmapping the region before that is a read of
    // freed memory on the data thread rather than an error anything reports.
    // The links and the sink carry object.linger=false, so they go with the
    // connection, but the region's name outlives the process and has to be taken
    // down by hand.
    if (d.filter != nullptr) {
        quiesce(d);
        pw_filter_destroy(d.filter);
        d.filter = nullptr;
    }
    unlink_output(d);
    for (pw_proxy* link : d.input_links) pw_proxy_destroy(link);
    if (d.virtual_sink != nullptr) pw_proxy_destroy(d.virtual_sink);
    close_region(d);
    if (d.metadata != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d.metadata));
    if (d.registry != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d.registry));
    if (d.core != nullptr) pw_core_disconnect(d.core);
    if (d.context != nullptr) pw_context_destroy(d.context);
    pw_main_loop_destroy(d.loop);

    pw_deinit();
    return 0;
}

}  // namespace isotone::daemon
