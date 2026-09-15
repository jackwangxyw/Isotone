// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicesmodel.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>

#include <objbase.h>

#include <algorithm>
#include <thread>

#include "config_files.h"
#include "device_watcher.h"
#include "devices.h"
#include "devicetoolrunner.h"
#include "eapo_install.h"
#include "equalizerapoconfig.h"

namespace {

constexpr int kPollMs = 3000;

// "config.txt · Isotone.txt" once config.txt includes Isotone.txt for every device.
QString config_line(bool included) {
    return included ? QStringLiteral("config.txt · Isotone.txt") : QStringLiteral("config.txt");
}

bool isotone_included() {
    const std::filesystem::path dir = equalizerApoConfigDir();
    return !dir.empty() && isotone::compat::inspect_config(dir).isotone_included;
}

void order(std::vector<DevicesModel::Entry>* entries) {
    std::stable_sort(entries->begin(), entries->end(), [](const DevicesModel::Entry& a, const DevicesModel::Entry& b) {
        if (a.facts.present != b.facts.present) return a.facts.present;
        if (a.facts.default_console != b.facts.default_console) return a.facts.default_console;
        return a.facts.name.compare(b.facts.name, Qt::CaseInsensitive) < 0;
    });
}

struct Reading {
    std::vector<DevicesModel::Entry> entries;
    bool eapo_installed = false;
    QString version, uninstaller;
};

// On a worker thread: the endpoints, each one's devicetool status, and Equalizer APO's install.
Reading read_machine() {
    Reading out;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<isotone::devices::Endpoint> endpoints;
    const HRESULT hr = isotone::devices::enumerate_render_endpoints(&endpoints);
    CoUninitialize();
    if (FAILED(hr)) return out;
    const std::wstring exe = devicetoolPath().toStdWString();
    const bool included = isotone_included();
    for (const isotone::devices::Endpoint& e : endpoints) {
        if ((e.state & DEVICE_STATE_NOTPRESENT) || e.guid.empty()) continue;
        DevicesModel::Entry entry;
        entry.facts = factsFromEndpoint(e);
        const DevicetoolResult r = runDevicetoolDirect(exe, {L"status", e.guid}, 30000);
        DeviceFacts status;
        if (r.error == ERROR_SUCCESS && factsFromStatusJson(QByteArray::fromStdString(r.json), &status)) {
            entry.facts.remedies = status.remedies;
            entry.facts.effect_slots = status.effect_slots;
            entry.facts.install_mode = status.install_mode;
            entry.facts.default_mode = status.default_mode;
            entry.status_json = QString::fromStdString(r.json);
        }
        if (equalizerApoPresent(entry.facts)) entry.config = config_line(included);
        out.entries.push_back(std::move(entry));
    }
    order(&out.entries);
    out.eapo_installed = isotone::compat::locate_equalizer_apo().error == ERROR_SUCCESS;
    const QSettings uninstall(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EqualizerAPO"),
                              QSettings::NativeFormat);
    out.version = uninstall.value(QStringLiteral("DisplayVersion")).toString();
    out.uninstaller = uninstall.value(QStringLiteral("UninstallString")).toString();
    return out;
}

}  // namespace

DevicesModel::DevicesModel(QObject* parent) : QAbstractListModel(parent) {
    const QString script = qEnvironmentVariable("ISOTONE_FAKE_DEVICETOOL");
    if (!script.isEmpty()) {
        QFile f(script);
        if (!f.open(QIODevice::ReadOnly) || !loadScript(QString::fromUtf8(f.readAll())))
            qWarning("ISOTONE_FAKE_DEVICETOOL: %s is not a script", qPrintable(script));
        return;
    }
    watcher_ = std::make_unique<isotone::devices::DeviceWatcher>();
    debounce_.setSingleShot(true);
    debounce_.setInterval(250);
    connect(&debounce_, &QTimer::timeout, this, &DevicesModel::refresh);
    // Callbacks arrive on an MMDevice thread: queue, and read once for a burst.
    QPointer<DevicesModel> self(this);
    watcher_->start([self](const isotone::devices::DeviceEvent&) {
        QMetaObject::invokeMethod(
            self.data(), [self] { if (self) self->debounce_.start(); }, Qt::QueuedConnection);
    });
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &DevicesModel::poll);
    poll_timer_.start();
    refresh();
}

DevicesModel::~DevicesModel() {
    *alive_ = false;
    if (watcher_) watcher_->stop();
}

int DevicesModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(entries_.size()); }

QVariant DevicesModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    const Entry& e = entries_[static_cast<size_t>(index.row())];
    const DeviceFacts& f = e.facts;
    switch (role) {
        case GuidRole: return f.guid;
        case NameRole: return f.name;
        case DefaultRole: return f.default_console;
        case PresentRole: return f.present;
        case StatusRole: return statusKey(f);
        case StatusLabelRole: return statusLabel(statusKey(f));
        case DotRole: return statusDot(statusKey(f));
        case EngineRole: return engineColumn(f);
        case EngineDetailRole: return engineDetail(f);
        case FormatRole: return formatLabel(f);
        case SlotRole: return f.effect_slots.isEmpty() ? QString(QChar(0x2014)) : f.effect_slots;
        case ConfigRole: return e.config;
        case NowRole: return nowEngine(f);
        case HasEqualizerApoRole: return equalizerApoPresent(f);
        case ActionsRole: return actions(f);
        case WorkingRole: return working(f);
    }
    return {};
}

QHash<int, QByteArray> DevicesModel::roleNames() const {
    return {{GuidRole, "guid"},         {NameRole, "name"},
            {DefaultRole, "isDefault"}, {PresentRole, "present"},
            {StatusRole, "status"},     {StatusLabelRole, "statusLabel"},
            {DotRole, "dot"},           {EngineRole, "engine"},
            {EngineDetailRole, "engineDetail"}, {FormatRole, "format"},
            {SlotRole, "slot"},         {ConfigRole, "config"},
            {NowRole, "now"},           {HasEqualizerApoRole, "hasEqualizerApo"},
            {ActionsRole, "actions"},   {WorkingRole, "working"}};
}

QString DevicesModel::defaultGuid() const {
    for (const Entry& e : entries_)
        if (e.facts.default_console) return e.facts.guid;
    return {};
}

bool DevicesModel::equalizerApoUsed() const {
    return std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) { return equalizerApoPresent(e.facts); });
}

const DevicesModel::Entry* DevicesModel::find(const QString& guid) const {
    for (const Entry& e : entries_)
        if (e.facts.guid.compare(guid, Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

int DevicesModel::indexOf(const QString& guid) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].facts.guid.compare(guid, Qt::CaseInsensitive) == 0) return static_cast<int>(i);
    return -1;
}

QVariantMap DevicesModel::row(const QString& guid) const {
    const int i = indexOf(guid);
    if (i < 0) return {};
    QVariantMap out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) out.insert(QString::fromLatin1(it.value()), data(index(i), it.key()));
    return out;
}

QVariantMap DevicesModel::operation(const QString& guid, const QString& action) const {
    const Entry* e = find(guid);
    return e ? ::operation(e->facts, action) : QVariantMap();
}

QVariantMap DevicesModel::plan(const QString& guid, const QString& want) const {
    const Entry* e = find(guid);
    return e ? planChange(e->facts, want) : QVariantMap();
}

void DevicesModel::copyDiagnostics(const QString& guid) const {
    const Entry* e = find(guid);
    if (!e) return;
    if (QClipboard* clipboard = QGuiApplication::clipboard()) clipboard->setText(e->status_json);
}

void DevicesModel::setEntries(std::vector<Entry> entries, bool eapo_installed, const QString& version, const QString& uninstaller) {
    beginResetModel();
    entries_ = std::move(entries);
    eapo_installed_ = eapo_installed;
    eapo_version_ = version;
    eapo_uninstaller_ = uninstaller;
    endResetModel();
    ++revision_;
    emit revisionChanged();
}

void DevicesModel::refresh() {
    if (fake_) {
        // A script's rows stay; only the config line is read again.
        const bool included = isotone_included();
        for (Entry& e : entries_)
            if (equalizerApoPresent(e.facts)) e.config = config_line(included);
        setEntries(std::move(entries_), eapo_installed_, eapo_version_, eapo_uninstaller_);
        return;
    }
    if (reading_) {
        read_again_ = true;
        return;
    }
    reading_ = true;
    QPointer<DevicesModel> self(this);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::thread([self, alive] {
        Reading reading = read_machine();
        if (!*alive) return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, reading = std::move(reading)]() mutable {
                if (!self) return;
                self->reading_ = false;
                if (!self->fake_) self->setEntries(std::move(reading.entries), reading.eapo_installed, reading.version, reading.uninstaller);
                if (std::exchange(self->read_again_, false)) self->refresh();
            },
            Qt::QueuedConnection);
    }).detach();
}

// Engine changes raise no notification: read each present output's engine,
// and read everything again when one changed.
void DevicesModel::poll() {
    if (fake_ || reading_ || polling_.exchange(true)) return;
    std::vector<std::pair<std::wstring, DeviceFacts>> known;
    for (const Entry& e : entries_) known.emplace_back(e.facts.guid.toStdWString(), e.facts);
    QPointer<DevicesModel> self(this);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    std::thread([self, alive, known] {
        bool changed = false;
        for (const auto& [guid, facts] : known) {
            const isotone::devices::EngineInfo engine = isotone::devices::read_engine(guid);
            changed |= engine.error == S_OK && (engine.isoapo_state != facts.isoapo || engine.backend != facts.backend ||
                                                engine.enhancements_disabled != facts.enhancements_disabled);
            if (!*alive) return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, changed] {
                if (!self) return;
                self->polling_ = false;
                if (changed) self->refresh();
            },
            Qt::QueuedConnection);
    }).detach();
}

bool DevicesModel::loadScript(const QString& script) {
    const QJsonDocument doc = QJsonDocument::fromJson(script.toUtf8());
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();
    std::vector<Entry> entries;
    const bool included = isotone_included();
    for (const QJsonValue& d : o.value(QStringLiteral("devices")).toArray()) {
        Entry e;
        const QByteArray json = QJsonDocument(d.toObject()).toJson(QJsonDocument::Compact);
        if (!factsFromStatusJson(json, &e.facts)) continue;
        e.status_json = QString::fromUtf8(json);
        if (equalizerApoPresent(e.facts)) e.config = config_line(included);
        entries.push_back(std::move(e));
    }
    order(&entries);
    const QJsonObject eapo = o.value(QStringLiteral("equalizer_apo")).toObject();
    fake_ = true;
    poll_timer_.stop();
    setEntries(std::move(entries), eapo.value(QStringLiteral("installed")).toBool(), eapo.value(QStringLiteral("version")).toString(),
               eapo.value(QStringLiteral("uninstaller")).toString());
    return true;
}
