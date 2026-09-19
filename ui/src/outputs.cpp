// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "outputs.h"

#include <QMetaObject>
#include <QPointer>

#include "speakers.h"

#if defined(_WIN32)
#include <objbase.h>

#include <thread>

#include "device_watcher.h"
#include "devices.h"
#include "engine_probe.h"
#include "equalizerapoconfig.h"
#include "shared_mapping.h"
#else
#include "daemon_region.h"
#include "pipewire_outputs.h"
#endif

namespace {

constexpr int kProbeIntervalMs = 3000;

}  // namespace

#if defined(_WIN32)
Outputs::Outputs(QObject* parent) : QAbstractListModel(parent), watcher_(std::make_unique<isotone::devices::DeviceWatcher>()) {
    refresh();
    // Callbacks arrive on an MMDevice thread and must not enumerate there: queue.
    QPointer<Outputs> self(this);
    watcher_->start([self](const isotone::devices::DeviceEvent&) {
        if (self) QMetaObject::invokeMethod(self, "refresh", Qt::QueuedConnection);
    });
    probe_timer_.setInterval(kProbeIntervalMs);
    connect(&probe_timer_, &QTimer::timeout, this, &Outputs::probe);
    probe_timer_.start();
    probe();
}

Outputs::~Outputs() {
    *alive_ = false;
    watcher_->stop();
}

void Outputs::refresh() {
    std::vector<isotone::devices::Endpoint> endpoints;
    if (FAILED(isotone::devices::enumerate_render_endpoints(&endpoints))) return;
    std::vector<Output> found;
    // Devices work package: an Equalizer APO output only while config.txt includes Isotone.txt and it is not turned Off.
    const bool attached = equalizerApoAttached();
    for (const isotone::devices::Endpoint& e : endpoints) {
        if (e.state != DEVICE_STATE_ACTIVE || !e.format.present) continue;
        const bool native = e.engine.backend == isotone::devices::Backend::native &&
                            e.engine.isoapo_state == isotone::devices::IsoApoState::installed;
        const bool eapo = e.engine.backend == isotone::devices::Backend::equalizerapo &&
                          equalizerApoOutputListed(QString::fromStdWString(e.guid), attached);
        if (!native && !eapo) continue;
        Output o;
        o.guid = isotone::ui::narrow_id(e.guid);
        o.name = QString::fromStdWString(e.connection_name.empty() ? e.friendly_name : e.connection_name) +
                 (e.device_name.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdWString(e.device_name)));
        o.backend = native ? isotone::ui::Backend::native : isotone::ui::Backend::equalizer_apo;
        o.layout = isotone::ui::OutputLayout{e.format.channels, e.format.channel_mask, static_cast<double>(e.format.sample_rate)};
        o.default_console = e.default_console;
        // Equalizer APO has no heartbeat: what is known is that it is in a slot.
        o.activity = eapo ? QStringLiteral("running") : QStringLiteral("unknown");
        for (const Output& old : outputs_)
            if (native && old.guid == o.guid) o.activity = old.activity;
        found.push_back(std::move(o));
    }

    const std::string previous = current_guid_;
    const Output* before = current();
    const isotone::ui::OutputLayout before_layout = before ? before->layout : isotone::ui::OutputLayout{};
    beginResetModel();
    outputs_ = std::move(found);
    endResetModel();
    emit countChanged();

    bool still_there = false;
    for (const Output& o : outputs_) still_there |= o.guid == previous;
    if (!still_there) {
        // The default output if it works, else the first that does.
        current_guid_.clear();
        for (const Output& o : outputs_)
            if (o.default_console) current_guid_ = o.guid;
        if (current_guid_.empty() && !outputs_.empty()) current_guid_ = outputs_.front().guid;
    }
    const Output* now = current();
    const bool format_moved = now && (now->layout.channels != before_layout.channels ||
                                      now->layout.speaker_mask != before_layout.speaker_mask ||
                                      now->layout.sample_rate != before_layout.sample_rate);
    if (current_guid_ != previous || format_moved) emit currentChanged();
    emit currentActivityChanged();

    // Settings, General: the default output moved.
    std::string default_guid;
    for (const isotone::devices::Endpoint& e : endpoints)
        if (e.state == DEVICE_STATE_ACTIVE && e.default_console)
            default_guid = isotone::ui::narrow_id(e.guid);
    const bool moved = refreshed_ && default_guid != default_guid_;
    default_guid_ = default_guid;
    refreshed_ = true;
    if (moved) emit defaultOutputChanged();
}

#else
Outputs::Outputs(QObject* parent) : QAbstractListModel(parent), pipewire_(std::make_unique<isotone::ui::PipewireOutputs>()) {
    // Called on PipeWire's thread: queue.
    QPointer<Outputs> self(this);
    pipewire_->set_on_changed([self] {
        if (self) QMetaObject::invokeMethod(self, "refresh", Qt::QueuedConnection);
    });
    // No PipeWire is an empty list, as a machine with no working output is.
    if (pipewire_->start()) pipewire_->wait_ready();
    refresh();
    probe_timer_.setInterval(kProbeIntervalMs);
    connect(&probe_timer_, &QTimer::timeout, this, &Outputs::probe);
    probe_timer_.start();
}

Outputs::~Outputs() { pipewire_->stop(); }

void Outputs::refresh() {
    const std::vector<isotone::ui::PipewireSink> sinks = pipewire_->sinks();
    std::string default_sink = pipewire_->default_sink();

    // The sink the daemon feeds has its region; the core's format, which every
    // output is edited at (one core, one virtual sink), is in its header.
    std::string fed;
    isotone::ui::DaemonRegion region;
    for (const isotone::ui::PipewireSink& sink : sinks) {
        if (sink.is_isotone || isotone::ui::read_daemon_region(sink.name, &region) != 0) continue;
        fed = sink.name;
        break;
    }
    isotone::ui::OutputLayout layout{2, 0x3, 48000.0};
    if (!fed.empty() && region.channels > 0) {
        layout = isotone::ui::OutputLayout{region.channels, region.speaker_mask,
                                           region.sample_rate > 0 ? static_cast<double>(region.sample_rate) : 48000.0};
    }
    // Isotone's own sink as the default is how applications come to play into
    // it: the output they are heard on is the one it feeds.
    for (const isotone::ui::PipewireSink& sink : sinks)
        if (sink.is_isotone && sink.name == default_sink) default_sink = fed;

    std::vector<Output> found;
    for (const isotone::ui::PipewireSink& sink : sinks) {
        // A socket with nothing in it, an unplugged HDMI, is left out, as an
        // unplugged endpoint is on Windows (owner, 2026-09-19).
        if (sink.is_isotone || !sink.connected) continue;
        Output o;
        o.guid = sink.name;
        o.name = QString::fromStdString(sink.description);
        o.backend = isotone::ui::Backend::pipewire;
        o.layout = layout;
        o.default_console = sink.name == default_sink;
        o.activity = QStringLiteral("idle");
        for (const Output& old : outputs_)
            if (old.guid == o.guid && o.guid == fed) o.activity = old.activity;
        found.push_back(std::move(o));
    }
    if (fed != fed_) heartbeat_ = region.heartbeat;
    fed_ = fed;

    const std::string previous = current_guid_;
    const Output* before = current();
    const isotone::ui::OutputLayout before_layout = before ? before->layout : isotone::ui::OutputLayout{};
    beginResetModel();
    outputs_ = std::move(found);
    endResetModel();
    emit countChanged();

    bool still_there = false;
    for (const Output& o : outputs_) still_there |= o.guid == previous;
    if (!still_there) {
        // The default output, else the first.
        current_guid_.clear();
        for (const Output& o : outputs_)
            if (o.default_console) current_guid_ = o.guid;
        if (current_guid_.empty() && !outputs_.empty()) current_guid_ = outputs_.front().guid;
    }
    const Output* now = current();
    const bool format_moved = now && (now->layout.channels != before_layout.channels ||
                                      now->layout.speaker_mask != before_layout.speaker_mask ||
                                      now->layout.sample_rate != before_layout.sample_rate);
    if (current_guid_ != previous || format_moved) emit currentChanged();
    emit currentActivityChanged();

    // Settings, General: the default output moved.
    const bool moved = refreshed_ && default_sink != default_guid_;
    default_guid_ = default_sink;
    refreshed_ = true;
    if (moved) emit defaultOutputChanged();
}
#endif

bool Outputs::selectDefault() {
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].guid != default_guid_) continue;
        select(static_cast<int>(i));
        return true;
    }
    return false;
}

int Outputs::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(outputs_.size()); }

QVariant Outputs::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    const Output& o = outputs_[static_cast<size_t>(index.row())];
    switch (role) {
        case NameRole: return o.name;
        case BackendLabelRole: {
            QString label = o.backend == isotone::ui::Backend::native     ? QStringLiteral("Native")
                            : o.backend == isotone::ui::Backend::pipewire ? QStringLiteral("PipeWire")
                                                                          : QStringLiteral("Equalizer APO");
            if (o.layout.channels > 2) label += QStringLiteral(" · %1").arg(speaker_layout_name(o.layout.channels, o.layout.speaker_mask));
            return label;
        }
        case ActivityRole: return o.activity;
        case CurrentRole: return o.guid == current_guid_;
    }
    return {};
}

QHash<int, QByteArray> Outputs::roleNames() const {
    return {{NameRole, "name"}, {BackendLabelRole, "backendLabel"}, {ActivityRole, "activity"}, {CurrentRole, "current"}};
}

int Outputs::currentRow() const {
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (outputs_[i].guid == current_guid_) return static_cast<int>(i);
    return -1;
}

const Outputs::Output* Outputs::current() const {
    const int row = currentRow();
    return row < 0 ? nullptr : &outputs_[static_cast<size_t>(row)];
}

QString Outputs::currentActivity() const {
    const Output* o = current();
    return o ? o->activity : QString();
}

QString Outputs::currentGuid() const {
    const Output* o = current();
    return o ? QString::fromStdString(o->guid) : QString();
}

QString Outputs::currentName() const {
    const Output* o = current();
    return o ? o->name : QString();
}

void Outputs::select(int row) {
    if (row < 0 || row >= rowCount() || outputs_[static_cast<size_t>(row)].guid == current_guid_) return;
    const int previous = currentRow();
    current_guid_ = outputs_[static_cast<size_t>(row)].guid;
    if (previous >= 0) emit dataChanged(index(previous), index(previous), {CurrentRole});
    emit dataChanged(index(row), index(row), {CurrentRole});
    emit currentChanged();
    emit currentActivityChanged();
}

bool Outputs::selectGuid(const std::string& endpoint) {
#if defined(_WIN32)
    const std::string guid =
        isotone::ui::narrow_id(isotone::win::canonical_endpoint_guid(isotone::ui::widen_id(endpoint)));
#else
    // A node.name has one spelling.
    const std::string& guid = endpoint;
#endif
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].guid != guid) continue;
        select(static_cast<int>(i));
        return true;
    }
    return false;
}

#if defined(_WIN32)
// probe_engine blocks for its sampling interval: every IsoAPO output in turn, on
// a thread of its own, one round at a time.
void Outputs::probe() {
    std::vector<std::string> native;
    for (const Output& o : outputs_)
        if (o.backend == isotone::ui::Backend::native) native.push_back(o.guid);
    if (native.empty() || probing_.exchange(true)) return;
    QPointer<Outputs> self(this);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::thread([self, alive, native, this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::pair<std::string, QString>> results;
        for (const std::string& guid : native) {
            QString activity = QStringLiteral("unknown");
            isotone::devices::EngineProbe result;
            if (SUCCEEDED(isotone::devices::probe_engine(isotone::ui::widen_id(guid), &result))) {
                switch (result.activity) {
                    case isotone::devices::EngineActivity::running: activity = QStringLiteral("running"); break;
                    case isotone::devices::EngineActivity::idle: activity = QStringLiteral("idle"); break;
                    case isotone::devices::EngineActivity::stalled: activity = QStringLiteral("stalled"); break;
                    default: break;
                }
            }
            results.emplace_back(guid, activity);
            if (!*alive) break;
        }
        CoUninitialize();
        if (!*alive) return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, results, this] {
                if (!self) return;
                probing_ = false;
                for (const auto& [guid, activity] : results) {
                    for (size_t i = 0; i < outputs_.size(); ++i) {
                        if (outputs_[i].guid != guid || outputs_[i].activity == activity) continue;
                        outputs_[i].activity = activity;
                        emit dataChanged(index(static_cast<int>(i)), index(static_cast<int>(i)), {ActivityRole});
                        if (guid == current_guid_) emit currentActivityChanged();
                    }
                }
            },
            Qt::QueuedConnection);
    }).detach();
}
#else
// The fed sink runs while the daemon's heartbeat moves between two probes; a
// sink with no audio is suspended by PipeWire, and its heartbeat stands still.
// Reading the header is a map and a copy, so it stays on this thread.
void Outputs::probe() {
    isotone::ui::DaemonRegion region;
    if (fed_.empty()) {
        // A daemon that has just started feeding an output: its region comes a
        // moment after the sinks the registry reported, with nothing to announce it.
        for (const Output& o : outputs_) {
            if (isotone::ui::read_daemon_region(o.guid, &region) != 0) continue;
            refresh();
            return;
        }
        return;
    }
    const Output* fed = nullptr;
    for (const Output& o : outputs_)
        if (o.guid == fed_) fed = &o;
    if (isotone::ui::read_daemon_region(fed_, &region) != 0 || !fed || region.channels != fed->layout.channels ||
        region.speaker_mask != fed->layout.speaker_mask) {
        // The daemon let it go, started feeding another, or came back with
        // another layout (Settings, General, Layout restarts it): the list is stale.
        refresh();
        return;
    }
    const QString activity = region.heartbeat != heartbeat_ ? QStringLiteral("running") : QStringLiteral("idle");
    heartbeat_ = region.heartbeat;
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].guid != fed_ || outputs_[i].activity == activity) continue;
        outputs_[i].activity = activity;
        emit dataChanged(index(static_cast<int>(i)), index(static_cast<int>(i)), {ActivityRole});
        if (fed_ == current_guid_) emit currentActivityChanged();
    }
}
#endif
