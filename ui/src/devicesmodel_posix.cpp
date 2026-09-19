// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Devices view on Linux. One daemon hosts the core for one sink at a time,
// so the rows say which sink it feeds rather than what is installed where:
//   fed      the daemon feeds it (its region exists)             ok
//   standby  the daemon runs and feeds another sink              off
//   stopped  no daemon: Isotone's own sink is not there          bad   Start
// The only action is Start, which asks systemd to start the user unit
// (devicetool_posix.cpp). Read on PipeWire's change notifications and every
// 3 s, since the daemon moving to another sink shows only in its regions.

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>

#include "daemon_region.h"
#include "devicesmodel.h"
#include "pipewire_outputs.h"

namespace {

constexpr int kPollIntervalMs = 3000;

QString status_label(const QString& status) {
    if (status == QLatin1String("fed")) return QStringLiteral("Active");
    if (status == QLatin1String("standby")) return QStringLiteral("Standby");
    return QStringLiteral("Daemon stopped");
}

QString status_dot(const QString& status) {
    if (status == QLatin1String("fed")) return QStringLiteral("ok");
    if (status == QLatin1String("standby")) return QStringLiteral("off");
    return QStringLiteral("bad");
}

QString format_label(const isotone::ui::DaemonRegion& r) {
    // "48 kHz · 2 ch", as the Windows rows read.
    const double khz = r.sample_rate / 1000.0;
    return QStringLiteral("%1 kHz · %2 ch").arg(QString::number(khz, 'g', 4)).arg(r.channels);
}

}  // namespace

DevicesModel::DevicesModel(QObject* parent) : QAbstractListModel(parent), pipewire_(std::make_unique<isotone::ui::PipewireOutputs>()) {
    QPointer<DevicesModel> self(this);
    // PipeWire's thread: queue, and let a burst of registry events read once.
    debounce_.setSingleShot(true);
    debounce_.setInterval(100);
    connect(&debounce_, &QTimer::timeout, this, &DevicesModel::refresh);
    pipewire_->set_on_changed([self] {
        if (self) QMetaObject::invokeMethod(&self->debounce_, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    if (pipewire_->start()) pipewire_->wait_ready();
    refresh();
    poll_timer_.setInterval(kPollIntervalMs);
    connect(&poll_timer_, &QTimer::timeout, this, &DevicesModel::poll);
    poll_timer_.start();
}

DevicesModel::~DevicesModel() {
    *alive_ = false;
    pipewire_->stop();
}

void DevicesModel::refresh() {
    const std::vector<isotone::ui::PipewireSink> sinks = pipewire_->sinks();
    std::string default_sink = pipewire_->default_sink();
    bool running = false;
    std::string fed;
    isotone::ui::DaemonRegion region;
    for (const isotone::ui::PipewireSink& sink : sinks) {
        if (sink.is_isotone) {
            running = true;
            continue;
        }
        if (fed.empty() && isotone::ui::read_daemon_region(sink.name, &region) == 0) fed = sink.name;
    }
    // As Outputs reads it: with Isotone's own sink the default, the output heard is the fed one.
    for (const isotone::ui::PipewireSink& sink : sinks)
        if (sink.is_isotone && sink.name == default_sink) default_sink = fed;

    std::vector<Entry> entries;
    for (const isotone::ui::PipewireSink& sink : sinks) {
        if (sink.is_isotone) continue;
        Entry e;
        e.guid = sink.name;
        e.name = QString::fromStdString(sink.description);
        e.is_default = sink.name == default_sink;
        e.status = !running ? QStringLiteral("stopped") : sink.name == fed ? QStringLiteral("fed") : QStringLiteral("standby");
        if (sink.name == fed && region.sample_rate > 0) e.format = format_label(region);
        entries.push_back(std::move(e));
    }
    setEntries(std::move(entries), false, QString(), QString());
}

void DevicesModel::poll() {
    // Only a change of which sink is fed needs a new read; the rest arrives as notifications.
    std::string fed;
    for (const Entry& e : entries_)
        if (e.status == QLatin1String("fed")) fed = e.guid;
    isotone::ui::DaemonRegion region;
    const bool still = !fed.empty() && isotone::ui::read_daemon_region(fed, &region) == 0;
    if (!still || fed.empty()) refresh();
}

void DevicesModel::setEntries(std::vector<Entry> entries, bool eapo_installed, const QString& version, const QString& uninstaller) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
    eapo_installed_ = eapo_installed;
    eapo_version_ = version;
    eapo_uninstaller_ = uninstaller;
    ++revision_;
    emit revisionChanged();
    emit outputsChanged();
}

const DevicesModel::Entry* DevicesModel::find(const QString& guid) const {
    for (const Entry& e : entries_)
        if (QString::fromStdString(e.guid) == guid) return &e;
    return nullptr;
}

int DevicesModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(entries_.size()); }

QVariant DevicesModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    const Entry& e = entries_[static_cast<size_t>(index.row())];
    const QString dash(QChar(0x2014));
    switch (role) {
        case GuidRole: return QString::fromStdString(e.guid);
        case NameRole: return e.name;
        case DefaultRole: return e.is_default;
        case PresentRole: return true;
        case StatusRole: return e.status;
        case StatusLabelRole: return status_label(e.status);
        case DotRole: return status_dot(e.status);
        case EngineRole: return QStringLiteral("PipeWire");
        case EngineDetailRole: return QStringLiteral("Isotone daemon");
        case FormatRole: return e.format.isEmpty() ? dash : e.format;
        case SlotRole: return QString();
        case ConfigRole: return QString();
        case NowRole: return QString();
        case HasEqualizerApoRole: return false;
        case ActionsRole: return e.status == QLatin1String("stopped") ? QStringList{QStringLiteral("start")} : QStringList{};
        case WorkingRole: return e.status != QLatin1String("stopped");
    }
    return {};
}

QHash<int, QByteArray> DevicesModel::roleNames() const {
    return {{GuidRole, "guid"},
            {NameRole, "name"},
            {DefaultRole, "isDefault"},
            {PresentRole, "present"},
            {StatusRole, "status"},
            {StatusLabelRole, "statusLabel"},
            {DotRole, "dot"},
            {EngineRole, "engine"},
            {EngineDetailRole, "engineDetail"},
            {FormatRole, "format"},
            {SlotRole, "slot"},
            {ConfigRole, "config"},
            {NowRole, "now"},
            {HasEqualizerApoRole, "hasEqualizerApo"},
            {ActionsRole, "actions"},
            {WorkingRole, "working"}};
}

QString DevicesModel::defaultGuid() const {
    for (const Entry& e : entries_)
        if (e.is_default) return QString::fromStdString(e.guid);
    return QString();
}

bool DevicesModel::equalizerApoUsed() const { return false; }

QVariantMap DevicesModel::row(const QString& guid) const {
    for (int i = 0; i < rowCount(); ++i) {
        if (QString::fromStdString(entries_[static_cast<size_t>(i)].guid) != guid) continue;
        QVariantMap out;
        const QHash<int, QByteArray> names = roleNames();
        for (auto it = names.cbegin(); it != names.cend(); ++it) out.insert(QString::fromLatin1(it.value()), data(index(i), it.key()));
        return out;
    }
    return {};
}

int DevicesModel::indexOf(const QString& guid) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (QString::fromStdString(entries_[i].guid) == guid) return static_cast<int>(i);
    return -1;
}

QVariantMap DevicesModel::operation(const QString& guid, const QString& action) const {
    const Entry* e = find(guid);
    if (!e || action != QLatin1String("start") || e->status != QLatin1String("stopped")) return {};
    return {{QStringLiteral("kind"), QStringLiteral("start")}, {QStringLiteral("args"), QStringList{}}};
}

// Settings Outputs has no Linux counterpart: nothing is chosen per output.
QVariantMap DevicesModel::plan(const QString&, const QString&) const { return {}; }

void DevicesModel::copyDiagnostics(const QString& guid) const {
    const Entry* e = find(guid);
    if (!e) return;
    QGuiApplication::clipboard()->setText(QStringLiteral("%1 (%2): %3%4\n")
                                              .arg(QString::fromStdString(e->guid), e->name, e->status,
                                                   e->format.isEmpty() ? QString() : QStringLiteral(", ") + e->format));
}

// Scripts describe devicetool's answers, which Linux has no counterpart of.
bool DevicesModel::loadScript(const QString&) { return false; }
