// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "daemon.h"

#include <pipewire/pipewire.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/types.h"
#include "persisted_state.h"
#include "shared_region.h"

namespace isotone::daemon {

namespace {

constexpr uint32_t kChannels = 2;

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

    pw_main_loop* loop    = nullptr;
    pw_context*   context = nullptr;
    pw_core*      core    = nullptr;
    pw_registry*  registry = nullptr;
    spa_hook      registry_hook{};

    pw_proxy*              virtual_sink = nullptr;
    std::vector<pw_proxy*> links;

    pw_filter* filter = nullptr;
    void*      in_port[kChannels]  = {};
    void*      out_port[kChannels] = {};

    std::map<uint32_t, NodeInfo> nodes;
    std::map<uint32_t, PortInfo> ports;
    bool linked = false;

    posix::SharedRegion region;
    std::string         region_name;

    Processor  processor;
    EqState    state;
    ParamBlock block{};
    uint32_t   applied_seq = 0;

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

    const uint32_t frames = request->frames > d->options.max_frames ? request->frames
                                                                    : d->options.max_frames;
    d->processor.initialize(request->rate, kChannels, frames);

    // from_param_block reuses this capacity, so applying a block on the audio
    // thread never allocates.
    d->state.bands.reserve(kParamMaxBands);

    ParamBlock* shared = d->region.params();
    if (shared != nullptr && param_block_read(shared, &d->block, 1000)) {
        d->applied_seq = d->block.hdr.seq;
        from_param_block(d->block, &d->state);
        remap_channels(&d->state, ChannelLayout{kChannels, 0});
    }
    d->processor.set_target(d->state);
    d->processor.reset();

    d->proc_rate = request->rate;
    d->proc_max_frames = frames;
    if (shared != nullptr) {
        host_publish_format(shared, request->rate, kChannels, 0, HostState::Running);
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

    float* in[kChannels]  = {};
    float* out[kChannels] = {};
    for (uint32_t c = 0; c < kChannels; ++c) {
        in[c]  = static_cast<float*>(pw_filter_get_dsp_buffer(d->in_port[c], frames));
        out[c] = static_cast<float*>(pw_filter_get_dsp_buffer(d->out_port[c], frames));
    }
    for (uint32_t c = 0; c < kChannels; ++c) {
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
            remap_channels(&d->state, ChannelLayout{kChannels, 0});
            d->processor.set_target(d->state);
        }
    }

    d->processor.process(out, frames);
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

const PortInfo* find_port(const Daemon& d, const std::string& node_name, const std::string& port_name,
                          bool is_output, uint32_t* id_out) {
    for (const auto& [id, port] : d.ports) {
        if (port.is_output != is_output || port.name != port_name) continue;
        const auto node = d.nodes.find(port.node_id);
        if (node == d.nodes.end() || node->second.name != node_name) continue;
        *id_out = id;
        return &port;
    }
    return nullptr;
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

bool make_link(Daemon& d, uint32_t output_port, uint32_t input_port) {
    pw_properties* props = pw_properties_new(nullptr, nullptr);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output_port);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input_port);
    pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");

    auto* link = static_cast<pw_proxy*>(pw_core_create_object(
        d.core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
    pw_properties_free(props);
    if (link == nullptr) return false;
    d.links.push_back(link);
    return true;
}

// Links monitor -> filter -> target sink once every port involved has appeared.
// Called whenever the registry learns something new; does nothing until it can
// do all four links, so a half-linked graph is never left behind.
void try_link(Daemon& d) {
    if (d.linked || d.filter == nullptr) return;

    const uint32_t filter_node = pw_filter_get_node_id(d.filter);
    if (filter_node == SPA_ID_INVALID) return;

    static const char* kMonitor[kChannels]  = {"monitor_FL", "monitor_FR"};
    static const char* kPlayback[kChannels] = {"playback_FL", "playback_FR"};
    static const char* kIn[kChannels]       = {"in_FL", "in_FR"};
    static const char* kOut[kChannels]      = {"out_FL", "out_FR"};

    uint32_t monitor[kChannels], filter_in[kChannels], filter_out[kChannels], playback[kChannels];
    for (uint32_t c = 0; c < kChannels; ++c) {
        if (find_port(d, d.options.sink_name, kMonitor[c], true, &monitor[c]) == nullptr) return;
        if (find_port(d, d.options.target_sink, kPlayback[c], false, &playback[c]) == nullptr) return;
        if (!find_port_by_node_id(d, filter_node, kIn[c], false, &filter_in[c])) return;
        if (!find_port_by_node_id(d, filter_node, kOut[c], true, &filter_out[c])) return;
    }

    for (uint32_t c = 0; c < kChannels; ++c) {
        if (!make_link(d, monitor[c], filter_in[c]) || !make_link(d, filter_out[c], playback[c])) {
            std::fprintf(stderr, "isotone-daemon: could not create a link\n");
            return;
        }
    }
    d.linked = true;
    std::fprintf(stderr, "isotone-daemon: %s -> core -> %s\n", d.options.sink_name.c_str(),
                 d.options.target_sink.c_str());
    std::fflush(stderr);
}

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
    }
}

void on_global_remove(void* data, uint32_t id) {
    auto* d = static_cast<Daemon*>(data);
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

// Fills a region this daemon creates with the sink's saved state, before any
// other process can see the region as valid. Flat when nothing is saved.
void seed_from_saved(ParamBlock* block, void* context) {
    const auto* saved = static_cast<const ParamBlock*>(context);
    if (saved == nullptr) return;
    const ParamBlockHeader header = block->hdr;
    *block = *saved;
    block->hdr = header;   // the live region's header is the host's, not the file's
}

bool open_region(Daemon& d) {
    d.region_name = posix::region_name(d.options.target_sink);
    if (d.region_name.empty()) {
        std::fprintf(stderr, "isotone-daemon: --sink is required\n");
        return false;
    }

    const std::string dir = d.options.state_dir.empty() ? posix::persisted_state_dir()
                                                        : d.options.state_dir;
    ParamBlock saved{};
    bool have_saved = false;
    if (!dir.empty()) {
        const std::string path = posix::persisted_state_path(dir, d.options.target_sink);
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
    return true;
}

bool create_virtual_sink(Daemon& d) {
    pw_properties* props = pw_properties_new(
        PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_NODE_NAME, d.options.sink_name.c_str(),
        PW_KEY_NODE_DESCRIPTION, d.options.sink_description.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
        "audio.position", "[ FL FR ]", "monitor.channel-volumes", "false", PW_KEY_OBJECT_LINGER,
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

    if (!open_region(d)) return 1;
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

    static const char* kIn[kChannels]  = {"in_FL", "in_FR"};
    static const char* kOut[kChannels] = {"out_FL", "out_FR"};
    for (uint32_t c = 0; c < kChannels; ++c) {
        d.in_port[c]  = add_dsp_port(d.filter, PW_DIRECTION_INPUT, kIn[c]);
        d.out_port[c] = add_dsp_port(d.filter, PW_DIRECTION_OUTPUT, kOut[c]);
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
    // with the connection, but the region's name outlives the process and has
    // to be taken down by hand.
    if (ParamBlock* shared = d.region.params(); shared != nullptr) {
        host_publish_format(shared, d.proc_rate, kChannels, 0, HostState::NotLoaded);
    }
    for (pw_proxy* link : d.links) pw_proxy_destroy(link);
    if (d.virtual_sink != nullptr) pw_proxy_destroy(d.virtual_sink);
    if (d.filter != nullptr) pw_filter_destroy(d.filter);
    if (d.registry != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d.registry));
    if (d.core != nullptr) pw_core_disconnect(d.core);
    if (d.context != nullptr) pw_context_destroy(d.context);
    pw_main_loop_destroy(d.loop);

    d.region.close();
    if (options.unlink_on_exit) posix::SharedRegion::unlink_region(d.region_name);
    pw_deinit();
    return 0;
}

}  // namespace isotone::daemon
