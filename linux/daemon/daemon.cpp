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
    spa_hook     metadata_hook{};

    pw_proxy*              virtual_sink = nullptr;
    std::vector<pw_proxy*> input_links;    // our sink's monitor into the core
    std::vector<pw_proxy*> output_links;   // the core into the sink being fed
    bool                   input_linked = false;

    pw_filter* filter = nullptr;
    void*      in_port[kMaxChannels]  = {};
    void*      out_port[kMaxChannels] = {};

    // Fixed once, before the filter connects, so the audio thread may read them.
    uint32_t      channels = 2;
    uint32_t      speaker_mask = 0;
    const Layout* layout = nullptr;

    std::map<uint32_t, NodeInfo> nodes;
    std::map<uint32_t, PortInfo> ports;

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
        if (d->ring.claim(d->ring_token) && d->ring.owns()) {
            host_publish_format(shared, request->rate, d->channels, d->speaker_mask,
                                HostState::Running);
        }
    }
    d->ready.store(true, std::memory_order_release);
    d->init_pending.store(false, std::memory_order_release);
    return 0;
}

// Audio thread (PW_FILTER_FLAG_RT_PROCESS).
void on_process(void* userdata, spa_io_position* position) {
    auto* d = static_cast<Daemon*>(userdata);

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

    const bool fits = d->ready.load(std::memory_order_acquire) && rate == d->proc_rate &&
                      frames <= d->proc_max_frames;
    if (!fits) {
        // Pass the audio through untouched and ask the main loop to resize. One
        // request at a time, or a stalled main loop would queue thousands.
        bool expected = false;
        if (d->init_pending.compare_exchange_strong(expected, true)) {
            d->ready.store(false, std::memory_order_release);
            const InitRequest request{rate, frames};
            pw_loop_invoke(pw_main_loop_get_loop(d->loop), do_initialize, 0, &request,
                           sizeof(request), false, d);
        }
        return;
    }

    ParamBlock* shared = d->region.params();
    if (shared != nullptr) {
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
void link_input(Daemon& d) {
    if (d.input_linked || d.filter == nullptr) return;
    const uint32_t filter_node = pw_filter_get_node_id(d.filter);
    if (filter_node == SPA_ID_INVALID) return;

    uint32_t monitor[kMaxChannels], filter_in[kMaxChannels];
    for (uint32_t c = 0; c < d.channels; ++c) {
        const Position& p = d.layout->positions[c];
        if (!find_port(d, d.options.sink_name, port_name("monitor_", p), true, &monitor[c])) return;
        if (!find_port_by_node_id(d, filter_node, port_name("in_", p), false, &filter_in[c])) return;
    }
    for (uint32_t c = 0; c < d.channels; ++c) {
        pw_proxy* link = make_link(d, monitor[c], filter_in[c]);
        if (link == nullptr) {
            std::fprintf(stderr, "isotone-daemon: could not link the monitor into the core\n");
            return;
        }
        d.input_links.push_back(link);
    }
    d.input_linked = true;
}

void unlink_output(Daemon& d) {
    for (pw_proxy* link : d.output_links) pw_proxy_destroy(link);
    d.output_links.clear();
}

// The core into the sink being fed. Redone whenever the target changes, and all
// or nothing: a half-linked graph would play one channel.
bool link_output(Daemon& d) {
    if (!d.output_links.empty() || d.filter == nullptr || d.target.empty()) return false;
    const uint32_t filter_node = pw_filter_get_node_id(d.filter);
    if (filter_node == SPA_ID_INVALID) return false;

    // A target that does not carry one of our positions simply does not get that
    // channel: a 5.1 core feeding a stereo sink still plays its front pair
    // rather than nothing at all. The front left has to land somewhere, though,
    // or there is no path worth making and the graph is better left alone.
    uint32_t filter_out[kMaxChannels], playback[kMaxChannels];
    bool     matched[kMaxChannels] = {};
    uint32_t matches = 0;
    for (uint32_t c = 0; c < d.channels; ++c) {
        const Position& p = d.layout->positions[c];
        if (!find_port_by_node_id(d, filter_node, port_name("out_", p), true, &filter_out[c]))
            return false;
        matched[c] = find_port(d, d.target, port_name("playback_", p), false, &playback[c]);
        if (matched[c]) ++matches;
    }
    if (matches == 0 || !matched[0]) return false;

    for (uint32_t c = 0; c < d.channels; ++c) {
        if (!matched[c]) continue;
        pw_proxy* link = make_link(d, filter_out[c], playback[c]);
        if (link == nullptr) {
            unlink_output(d);
            std::fprintf(stderr, "isotone-daemon: could not link the core into %s\n", d.target.c_str());
            return false;
        }
        d.output_links.push_back(link);
    }
    if (matches != d.channels) {
        std::fprintf(stderr, "isotone-daemon: %s carries %u of our %u channels\n", d.target.c_str(),
                     matches, d.channels);
    }
    std::fprintf(stderr, "isotone-daemon: %s -> core -> %s\n", d.options.sink_name.c_str(),
                 d.target.c_str());
    std::fflush(stderr);
    return true;
}

void try_link(Daemon& d) {
    link_input(d);
    if (d.output_links.empty()) link_output(d);
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

void close_region(Daemon& d) {
    if (!d.region.is_open()) return;
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
    if (subject != PW_ID_CORE || key == nullptr) return 0;
    if (std::strcmp(key, "default.audio.sink") != 0) return 0;
    set_target(*d, default_sink_name(value));
    return 0;
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
        d->nodes[id] = info;
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

    // Only when following: with --sink the target is the owner's choice and the
    // session manager does not get a vote.
    if (d->options.target_sink.empty() && d->metadata == nullptr &&
        std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, "metadata.name");
        if (name == nullptr || std::strcmp(name, "default") != 0) return;
        d->metadata =
            static_cast<pw_metadata*>(pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0));
        if (d->metadata != nullptr)
            pw_metadata_add_listener(d->metadata, &d->metadata_hook, &kMetadataEvents, d);
    }
}

void on_global_remove(void* data, uint32_t id) {
    auto* d = static_cast<Daemon*>(data);
    // The sink being fed going away takes its links with it, so they are dropped
    // rather than destroyed; the next default says where to go instead.
    const auto node = d->nodes.find(id);
    if (node != d->nodes.end() && !d->target.empty() && node->second.name == d->target) {
        d->output_links.clear();
        d->target.clear();
    }
    d->nodes.erase(id);
    d->ports.erase(id);
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

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
    if (!create_virtual_sink(d)) return 1;

    d.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(d.loop), "isotone",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter",
                          PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_NODE_NAME, "isotone-core",
                          PW_KEY_NODE_DESCRIPTION, "Isotone", PW_KEY_NODE_AUTOCONNECT, "false",
                          nullptr),
        &kFilterEvents, &d);
    if (d.filter == nullptr) {
        std::fprintf(stderr, "isotone-daemon: pw_filter_new_simple failed\n");
        return 1;
    }

    for (uint32_t c = 0; c < d.channels; ++c) {
        const Position& p = d.layout->positions[c];
        d.in_port[c]  = add_dsp_port(d.filter, PW_DIRECTION_INPUT, port_name("in_", p).c_str());
        d.out_port[c] = add_dsp_port(d.filter, PW_DIRECTION_OUTPUT, port_name("out_", p).c_str());
        if (d.in_port[c] == nullptr || d.out_port[c] == nullptr) {
            std::fprintf(stderr, "isotone-daemon: pw_filter_add_port failed\n");
            return 1;
        }
    }
    if (pw_filter_connect(d.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::fprintf(stderr, "isotone-daemon: pw_filter_connect failed\n");
        return 1;
    }

    d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d.registry, &d.registry_hook, &kRegistryEvents, &d);

    pw_main_loop_run(d.loop);

    // Teardown. The links and the sink carry object.linger=false, so they go
    // with the connection, but the region's name outlives the process and has to
    // be taken down by hand.
    close_region(d);
    unlink_output(d);
    for (pw_proxy* link : d.input_links) pw_proxy_destroy(link);
    if (d.virtual_sink != nullptr) pw_proxy_destroy(d.virtual_sink);
    if (d.filter != nullptr) pw_filter_destroy(d.filter);
    if (d.metadata != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d.metadata));
    if (d.registry != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d.registry));
    if (d.core != nullptr) pw_core_disconnect(d.core);
    if (d.context != nullptr) pw_context_destroy(d.context);
    pw_main_loop_destroy(d.loop);

    pw_deinit();
    return 0;
}

}  // namespace isotone::daemon
