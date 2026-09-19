// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "pipewire_outputs.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/json.h>

#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace isotone::ui {

namespace {

// The daemon's virtual sink, which is an output to play into and never one to
// feed. Kept in step with Options::sink_name in linux/daemon/daemon.h.
constexpr const char* kIsotoneSinkName = "isotone";

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

}  // namespace

struct PipewireOutputsImpl {
    pw_thread_loop* loop     = nullptr;
    pw_context*     context  = nullptr;
    pw_core*        core     = nullptr;
    pw_registry*    registry = nullptr;
    spa_hook        registry_hook{};
    pw_metadata*    metadata = nullptr;
    uint32_t        metadata_id = SPA_ID_INVALID;
    spa_hook        metadata_hook{};

    // Guards everything a caller can read. The PipeWire thread writes it.
    mutable std::mutex           mutex;
    std::map<uint32_t, PipewireSink> sinks;   // by registry id
    std::string                  default_name;

    std::function<void()> on_changed;

    // The first sweep is done when the server has answered a sync, not when the
    // registry has merely started reporting: there is no other way to know that
    // what is there has all arrived.
    std::mutex              ready_mutex;
    std::condition_variable ready_wake;
    bool                    ready = false;
    int                     sync_seq = 0;
    // The default metadata is bound during the sweep and its properties come
    // after the sweep's answer, so a second sync is what says the default is in.
    bool                    metadata_synced = false;
    spa_hook                core_hook{};

    void changed() {
        if (on_changed) on_changed();
    }
};

namespace {

void on_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
               uint32_t /*version*/, const spa_dict* props) {
    auto* d = static_cast<PipewireOutputsImpl*>(data);
    if (props == nullptr) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (media_class == nullptr || std::strcmp(media_class, "Audio/Sink") != 0) return;
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (name == nullptr) return;

        PipewireSink sink;
        sink.name = name;
        if (const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION))
            sink.description = description;
        if (sink.description.empty()) sink.description = sink.name;
        sink.is_isotone = sink.name == kIsotoneSinkName;
        {
            const std::lock_guard<std::mutex> lock(d->mutex);
            d->sinks[id] = std::move(sink);
        }
        d->changed();
        return;
    }

    if (d->metadata == nullptr && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, "metadata.name");
        if (name == nullptr || std::strcmp(name, "default") != 0) return;
        d->metadata =
            static_cast<pw_metadata*>(pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0));
        if (d->metadata == nullptr) return;
        d->metadata_id = id;
        static const pw_metadata_events kMetadataEvents = {
            .version = PW_VERSION_METADATA_EVENTS,
            .property = [](void* data, uint32_t subject, const char* key, const char* /*type*/,
                           const char* value) -> int {
                auto* impl = static_cast<PipewireOutputsImpl*>(data);
                if (subject != PW_ID_CORE || key == nullptr) return 0;
                if (std::strcmp(key, "default.audio.sink") != 0) return 0;
                {
                    const std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->default_name = default_sink_name(value);
                }
                impl->changed();
                return 0;
            },
        };
        pw_metadata_add_listener(d->metadata, &d->metadata_hook, &kMetadataEvents, d);
    }
}

void on_global_remove(void* data, uint32_t id) {
    auto* d = static_cast<PipewireOutputsImpl*>(data);
    // A metadata object that goes away must be forgotten, or the replacement
    // after a WirePlumber restart is never bound and the default stops moving.
    if (d->metadata != nullptr && d->metadata_id == id) {
        spa_hook_remove(&d->metadata_hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(d->metadata));
        d->metadata = nullptr;
        d->metadata_id = SPA_ID_INVALID;
    }
    bool removed = false;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        removed = d->sinks.erase(id) > 0;
    }
    if (removed) d->changed();
}

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

void on_core_done(void* data, uint32_t id, int seq) {
    auto* d = static_cast<PipewireOutputsImpl*>(data);
    if (id != PW_ID_CORE || seq != d->sync_seq) return;
    if (d->metadata != nullptr && !d->metadata_synced) {
        d->metadata_synced = true;
        d->sync_seq = pw_core_sync(d->core, PW_ID_CORE, 0);
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(d->ready_mutex);
        d->ready = true;
    }
    d->ready_wake.notify_all();
}

const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = on_core_done,
    .ping = nullptr,
    .error = nullptr,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
    .bound_props = nullptr,
};

}  // namespace

PipewireOutputs::PipewireOutputs() : impl_(std::make_unique<PipewireOutputsImpl>()) {}

PipewireOutputs::~PipewireOutputs() { stop(); }

void PipewireOutputs::set_on_changed(std::function<void()> on_changed) {
    impl_->on_changed = std::move(on_changed);
}

bool PipewireOutputs::start() {
    if (impl_->loop != nullptr) return true;

    // A second start sweeps the registry again, so the first sweep is not done
    // until that one answers. Without this, wait_ready() returns at once on the
    // strength of the previous run and the caller reads an empty list.
    {
        const std::lock_guard<std::mutex> lock(impl_->ready_mutex);
        impl_->ready = false;
    }
    impl_->metadata_synced = false;

    pw_init(nullptr, nullptr);
    impl_->loop = pw_thread_loop_new("isotone-outputs", nullptr);
    if (impl_->loop == nullptr) return false;

    pw_thread_loop_lock(impl_->loop);
    if (pw_thread_loop_start(impl_->loop) < 0) {
        pw_thread_loop_unlock(impl_->loop);
        stop();
        return false;
    }

    impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
    impl_->core = impl_->context != nullptr ? pw_context_connect(impl_->context, nullptr, 0) : nullptr;
    if (impl_->core == nullptr) {
        pw_thread_loop_unlock(impl_->loop);
        stop();
        return false;
    }

    pw_core_add_listener(impl_->core, &impl_->core_hook, &kCoreEvents, impl_.get());
    impl_->registry = pw_core_get_registry(impl_->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(impl_->registry, &impl_->registry_hook, &kRegistryEvents, impl_.get());
    // Answered once everything already in the registry has been reported.
    impl_->sync_seq = pw_core_sync(impl_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(impl_->loop);
    return true;
}

void PipewireOutputs::stop() {
    if (impl_->loop == nullptr) return;

    pw_thread_loop_lock(impl_->loop);
    if (impl_->metadata != nullptr) {
        spa_hook_remove(&impl_->metadata_hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(impl_->metadata));
        impl_->metadata = nullptr;
    }
    if (impl_->registry != nullptr) {
        spa_hook_remove(&impl_->registry_hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(impl_->registry));
        impl_->registry = nullptr;
    }
    if (impl_->core != nullptr) {
        spa_hook_remove(&impl_->core_hook);
        pw_core_disconnect(impl_->core);
        impl_->core = nullptr;
    }
    pw_thread_loop_unlock(impl_->loop);

    pw_thread_loop_stop(impl_->loop);
    if (impl_->context != nullptr) {
        pw_context_destroy(impl_->context);
        impl_->context = nullptr;
    }
    pw_thread_loop_destroy(impl_->loop);
    impl_->loop = nullptr;

    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sinks.clear();
    impl_->default_name.clear();
}

bool PipewireOutputs::running() const { return impl_->loop != nullptr && impl_->core != nullptr; }

std::vector<PipewireSink> PipewireOutputs::sinks() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<PipewireSink> out;
    out.reserve(impl_->sinks.size());
    for (const auto& [id, sink] : impl_->sinks) out.push_back(sink);
    return out;
}

std::string PipewireOutputs::default_sink() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->default_name;
}

bool PipewireOutputs::wait_ready(int timeout_ms) {
    if (impl_->loop == nullptr) return false;
    std::unique_lock<std::mutex> lock(impl_->ready_mutex);
    return impl_->ready_wake.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [this] { return impl_->ready; });
}

}  // namespace isotone::ui
