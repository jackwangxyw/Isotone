// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "outputs.h"

#include <QMetaObject>
#include <QPointer>

#include <objbase.h>

#include <thread>

#include "device_watcher.h"
#include "devices.h"
#include "engine_probe.h"
#include "shared_mapping.h"

namespace {

constexpr int kProbeIntervalMs = 3000;

}  // namespace

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
    for (const isotone::devices::Endpoint& e : endpoints) {
        if (e.state != DEVICE_STATE_ACTIVE || !e.format.present) continue;
        const bool native = e.engine.backend == isotone::devices::Backend::native &&
                            e.engine.isoapo_state == isotone::devices::IsoApoState::installed;
        const bool eapo = e.engine.backend == isotone::devices::Backend::equalizerapo;
        if (!native && !eapo) continue;
        Output o;
        o.guid = e.guid;
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

    const std::wstring previous = current_guid_;
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
}

int Outputs::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(outputs_.size()); }

QVariant Outputs::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    const Output& o = outputs_[static_cast<size_t>(index.row())];
    switch (role) {
        case NameRole: return o.name;
        case BackendLabelRole: {
            QString label = o.backend == isotone::ui::Backend::native ? QStringLiteral("Native") : QStringLiteral("Equalizer APO");
            if (o.layout.channels > 2) label += QStringLiteral(" · %1").arg(o.layout.channels == 8 ? QStringLiteral("7.1") : QStringLiteral("5.1"));
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

bool Outputs::selectGuid(const std::wstring& endpoint) {
    const std::wstring guid = isotone::win::canonical_endpoint_guid(endpoint);
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].guid != guid) continue;
        select(static_cast<int>(i));
        return true;
    }
    return false;
}

// probe_engine blocks for its sampling interval: every IsoAPO output in turn, on
// a thread of its own, one round at a time.
void Outputs::probe() {
    std::vector<std::wstring> native;
    for (const Output& o : outputs_)
        if (o.backend == isotone::ui::Backend::native) native.push_back(o.guid);
    if (native.empty() || probing_.exchange(true)) return;
    QPointer<Outputs> self(this);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::thread([self, alive, native, this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::pair<std::wstring, QString>> results;
        for (const std::wstring& guid : native) {
            QString activity = QStringLiteral("unknown");
            isotone::devices::EngineProbe result;
            if (SUCCEEDED(isotone::devices::probe_engine(guid, &result))) {
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
