// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Every render endpoint for the Devices view, Settings Outputs and first run:
// active ones first, then disabled and unplugged ones (dimmed); endpoints whose
// hardware is gone (DEVICE_STATE_NOTPRESENT) are left out. Status and engine
// from windows/devices (devicestatus.h); each output's slots and remedies from
// `isotone-devicetool status`, run unelevated. Read on a worker thread, again
// on device notifications, after devicetool runs (Main.qml) and when the engine
// state polled every 3 s changes, since engine changes raise no notification.
//
// ISOTONE_FAKE_DEVICETOOL names a script whose "devices" (status JSON objects)
// and "equalizer_apo" ({"installed", "version", "uninstaller"}) replace the
// machine's (tests, screenshots).

#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

#include "devicestatus.h"

namespace isotone::devices {
class DeviceWatcher;
}

class DevicesModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(Devices)
    QML_SINGLETON

    Q_PROPERTY(int count READ rowCount NOTIFY revisionChanged)
    // Bumped whenever the rows are read again; bind to it to re-evaluate row().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString defaultGuid READ defaultGuid NOTIFY revisionChanged)
    Q_PROPERTY(bool equalizerApoInstalled READ equalizerApoInstalled NOTIFY revisionChanged)
    Q_PROPERTY(QString equalizerApoVersion READ equalizerApoVersion NOTIFY revisionChanged)
    // Equalizer APO is in a slot of some output, or holds IsoAPO's.
    Q_PROPERTY(bool equalizerApoUsed READ equalizerApoUsed NOTIFY revisionChanged)

public:
    enum Role {
        GuidRole = Qt::UserRole + 1,
        NameRole,
        DefaultRole,
        PresentRole,
        StatusRole,        // devicestatus.h's key
        StatusLabelRole,
        DotRole,
        EngineRole,        // the table's column
        EngineDetailRole,
        FormatRole,
        SlotRole,
        ConfigRole,        // "config.txt · Isotone.txt" or "config.txt" for Equalizer APO outputs, else empty
        NowRole,           // Settings Outputs' Now
        HasEqualizerApoRole,
        ActionsRole,
        WorkingRole,
    };

    explicit DevicesModel(QObject* parent = nullptr);
    ~DevicesModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int revision() const { return revision_; }
    QString defaultGuid() const;
    bool equalizerApoInstalled() const { return eapo_installed_; }
    QString equalizerApoVersion() const { return eapo_version_; }
    bool equalizerApoUsed() const;

    Q_INVOKABLE void refresh();
    // Every role of the output, by role name; empty when there is none.
    Q_INVOKABLE QVariantMap row(const QString& guid) const;
    Q_INVOKABLE int indexOf(const QString& guid) const;
    Q_INVOKABLE QVariantMap operation(const QString& guid, const QString& action) const;
    Q_INVOKABLE QVariantMap plan(const QString& guid, const QString& want) const;
    // devicetool status's JSON for the output (Copy diagnostics).
    Q_INVOKABLE void copyDiagnostics(const QString& guid) const;
    Q_INVOKABLE void openEqualizerApoUninstaller() { emit equalizerApoUninstallerRequested(eapo_uninstaller_); }
    // Replaces the rows with a script's (tests). False when it does not parse.
    Q_INVOKABLE bool loadScript(const QString& script);

    struct Entry {
        DeviceFacts facts;
        QString status_json;
        QString config;
    };

signals:
    void revisionChanged();
    // A read found an output's engine, config.txt's include or its Off setting
    // changed: what Outputs lists may have changed too (Main.qml refreshes it).
    void outputsChanged();
    // The app connects it (main.cpp); tests cannot run the uninstaller.
    void equalizerApoUninstallerRequested(const QString& command);

private:
    void setEntries(std::vector<Entry> entries, bool eapo_installed, const QString& version, const QString& uninstaller);
    void poll();
    const Entry* find(const QString& guid) const;

    std::vector<Entry> entries_;
    int revision_ = 0;
    bool eapo_installed_ = false;
    QString eapo_version_, eapo_uninstaller_;
    bool fake_ = false;

    std::unique_ptr<isotone::devices::DeviceWatcher> watcher_;
    QTimer poll_timer_;
    QTimer debounce_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool reading_ = false;
    bool read_again_ = false;
    std::atomic<bool> polling_{false};
};
