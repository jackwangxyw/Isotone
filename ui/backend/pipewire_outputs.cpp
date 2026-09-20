// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "pipewire_outputs.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
// spa_pod_get_array, which parser.h pulled in up to PipeWire 1.4 and does not
// in 1.6: without this the UI backend stops building on Ubuntu 26.04
// (measured on the GNOME VM, pipewire 1.6.2).
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>

#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
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

    // The cards, for the routes that say what is plugged in. Bound for as long
    // as the card is there; a route changes when a cable is (param event).
    struct Card {
        uint32_t               id = SPA_ID_INVALID;   // its registry id, the sinks' device.id
        pw_device*             proxy = nullptr;
        spa_hook               hook{};
        std::vector<CardRoute> routes;
        PipewireOutputsImpl*   owner = nullptr;
    };
    std::map<uint32_t, std::unique_ptr<Card>> cards;   // by registry id

    // A bound sink, for card.profile.device: the registry's properties carry
    // device.id but not which of the card's devices the sink is, and that is what
    // a route names.
    struct Sink {
        uint32_t             id = SPA_ID_INVALID;
        pw_node*             proxy = nullptr;
        spa_hook             hook{};
        PipewireOutputsImpl* owner = nullptr;
    };
    std::map<uint32_t, std::unique_ptr<Sink>> bound_sinks;   // by registry id

    // Guards everything a caller can read. The PipeWire thread writes it.
    mutable std::mutex           mutex;
    std::map<uint32_t, PipewireSink> sinks;   // by registry id
    // Each sink's card (device.id) and which of its devices it is
    // (card.profile.device), so a route can be matched to it.
    struct SinkCard {
        uint32_t device_id = SPA_ID_INVALID;
        uint32_t card_device = SPA_ID_INVALID;
    };
    std::map<uint32_t, SinkCard> sink_cards;   // by the sink's registry id
    std::map<uint32_t, std::vector<CardRoute>> card_routes;   // by the card's device.id
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

bool sink_connected(const std::vector<CardRoute>& routes, uint32_t card_device) {
    bool any_for_this_device = false;
    for (const CardRoute& r : routes) {
        if (r.card_device != card_device) continue;
        any_for_this_device = true;
        if (r.available) return true;
    }
    return !any_for_this_device;
}

std::string sink_display_name(const char* nick, const char* description, const std::string& name) {
    if (nick != nullptr && *nick != '\0') return nick;
    if (description != nullptr && *description != '\0') return description;
    return name;
}

namespace {

// A card's routes: which of its devices each is for, and whether it reports
// something plugged in (SPA_PARAM_AVAILABILITY_no is an empty socket, an
// unplugged HDMI; "unknown" is a route that cannot tell, and counts as plugged).
void on_card_param(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/,
                   const spa_pod* param) {
    auto* card = static_cast<PipewireOutputsImpl::Card*>(data);
    if (id != SPA_PARAM_EnumRoute || param == nullptr) return;
    uint32_t available = SPA_PARAM_AVAILABILITY_unknown;
    spa_pod* devices = nullptr;
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, nullptr,
                             SPA_PARAM_ROUTE_available, SPA_POD_OPT_Id(&available),
                             SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Pod(&devices)) < 0) {
        return;
    }
    if (devices == nullptr || !spa_pod_is_array(devices)) return;
    uint32_t n = 0;
    const int32_t* values = static_cast<int32_t*>(spa_pod_get_array(devices, &n));
    if (values == nullptr) return;
    for (uint32_t i = 0; i < n; ++i) {
        card->routes.push_back(CardRoute{static_cast<uint32_t>(values[i]),
                                         available != SPA_PARAM_AVAILABILITY_no});
    }
    // Published as each arrives rather than at the end of the enumeration: there
    // is no event that says it ended, and a route more can only free an output.
    {
        const std::lock_guard<std::mutex> lock(card->owner->mutex);
        card->owner->card_routes[card->id] = card->routes;
    }
    card->owner->changed();
}

// Every info carries the routes again: a cable in or out changes one, and the
// server says so by sending the device's info with EnumRoute among its params.
void on_card_info(void* data, const pw_device_info* info) {
    auto* card = static_cast<PipewireOutputsImpl::Card*>(data);
    bool routes = false;
    for (uint32_t i = 0; i < info->n_params; ++i)
        if (info->params[i].id == SPA_PARAM_EnumRoute) routes = true;
    if (!routes) return;
    card->routes.clear();
    pw_device_enum_params(card->proxy, 0, SPA_PARAM_EnumRoute, 0, UINT32_MAX, nullptr);
}

const pw_device_events kCardEvents = {
    .version = PW_VERSION_DEVICE_EVENTS,
    .info = on_card_info,
    .param = on_card_param,
};

void on_sink_info(void* data, const pw_node_info* info) {
    auto* sink = static_cast<PipewireOutputsImpl::Sink*>(data);
    if (info->props == nullptr) return;
    const char* card_device = spa_dict_lookup(info->props, "card.profile.device");
    if (card_device == nullptr) return;
    {
        const std::lock_guard<std::mutex> lock(sink->owner->mutex);
        sink->owner->sink_cards[sink->id].card_device =
            static_cast<uint32_t>(std::strtoul(card_device, nullptr, 10));
    }
    sink->owner->changed();
}

const pw_node_events kSinkEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = on_sink_info,
    .param = nullptr,
};

void bind_sink(PipewireOutputsImpl& d, uint32_t id) {
    if (d.bound_sinks.count(id) != 0) return;
    auto sink = std::make_unique<PipewireOutputsImpl::Sink>();
    sink->id = id;
    sink->owner = &d;
    sink->proxy = static_cast<pw_node*>(
        pw_registry_bind(d.registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (sink->proxy == nullptr) return;
    pw_node_add_listener(sink->proxy, &sink->hook, &kSinkEvents, sink.get());
    d.bound_sinks[id] = std::move(sink);
}

void bind_card(PipewireOutputsImpl& d, uint32_t id) {
    if (d.cards.count(id) != 0) return;
    auto card = std::make_unique<PipewireOutputsImpl::Card>();
    card->id = id;
    card->owner = &d;
    card->proxy = static_cast<pw_device*>(
        pw_registry_bind(d.registry, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
    if (card->proxy == nullptr) return;
    pw_device_add_listener(card->proxy, &card->hook, &kCardEvents, card.get());
    d.cards[id] = std::move(card);
}

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
        sink.description = sink_display_name(spa_dict_lookup(props, PW_KEY_NODE_NICK),
                                             spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION), sink.name);
        sink.is_isotone = sink.name == kIsotoneSinkName;
        PipewireOutputsImpl::SinkCard card;
        if (const char* device_id = spa_dict_lookup(props, PW_KEY_DEVICE_ID))
            card.device_id = static_cast<uint32_t>(std::strtoul(device_id, nullptr, 10));
        if (const char* card_device = spa_dict_lookup(props, "card.profile.device"))
            card.card_device = static_cast<uint32_t>(std::strtoul(card_device, nullptr, 10));
        {
            const std::lock_guard<std::mutex> lock(d->mutex);
            d->sinks[id] = std::move(sink);
            d->sink_cards[id].device_id = card.device_id;
        }
        if (card.device_id != SPA_ID_INVALID) bind_sink(*d, id);
        d->changed();
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
        const char* api = spa_dict_lookup(props, PW_KEY_DEVICE_API);
        const char* media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (media == nullptr || std::strcmp(media, "Audio/Device") != 0 || api == nullptr) return;
        bind_card(*d, id);
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
    if (auto card = d->cards.find(id); card != d->cards.end()) {
        spa_hook_remove(&card->second->hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(card->second->proxy));
        d->cards.erase(card);
        {
            const std::lock_guard<std::mutex> lock(d->mutex);
            d->card_routes.erase(id);
        }
        d->changed();
    }
    if (auto sink = d->bound_sinks.find(id); sink != d->bound_sinks.end()) {
        spa_hook_remove(&sink->second->hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(sink->second->proxy));
        d->bound_sinks.erase(sink);
    }
    bool removed = false;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        removed = d->sinks.erase(id) > 0;
        d->sink_cards.erase(id);
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
    for (auto& [id, card] : impl_->cards) {
        spa_hook_remove(&card->hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(card->proxy));
    }
    impl_->cards.clear();
    for (auto& [id, sink] : impl_->bound_sinks) {
        spa_hook_remove(&sink->hook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(sink->proxy));
    }
    impl_->bound_sinks.clear();
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
    impl_->sink_cards.clear();
    impl_->card_routes.clear();
    impl_->default_name.clear();
}

bool PipewireOutputs::running() const { return impl_->loop != nullptr && impl_->core != nullptr; }

std::vector<PipewireSink> PipewireOutputs::sinks() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<PipewireSink> out;
    out.reserve(impl_->sinks.size());
    for (const auto& [id, sink] : impl_->sinks) {
        PipewireSink copy = sink;
        const auto card = impl_->sink_cards.find(id);
        if (card != impl_->sink_cards.end() && card->second.device_id != SPA_ID_INVALID &&
            card->second.card_device != SPA_ID_INVALID) {
            const auto routes = impl_->card_routes.find(card->second.device_id);
            copy.connected = routes == impl_->card_routes.end() ||
                             sink_connected(routes->second, card->second.card_device);
        }
        out.push_back(std::move(copy));
    }
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
