// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The outputs in the sidebar: render endpoints whose engine works (IsoAPO
// installed, or Equalizer APO in a slot), active (owner's decision, 2026-09-14).
// Refreshed on device notifications; the current output's engine is probed off
// the UI thread every few seconds for its status dot.

#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

#include "devicelink.h"

namespace isotone::devices {
class DeviceWatcher;
}

class Outputs : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int currentRow READ currentRow NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    // "running", "idle", "stalled" or "unknown"; the collapsed rail's dot.
    Q_PROPERTY(QString currentActivity READ currentActivity NOTIFY currentActivityChanged)
    // The current output's braced GUID, empty when no output works (Devices work package: the top bar's status pill).
    Q_PROPERTY(QString currentGuid READ currentGuid NOTIFY currentChanged)

public:
    enum Role { NameRole = Qt::UserRole + 1, BackendLabelRole, ActivityRole, CurrentRole };

    struct Output {
        std::wstring guid;
        QString name;
        isotone::ui::Backend backend = isotone::ui::Backend::none;
        isotone::ui::OutputLayout layout;
        bool default_console = false;
        QString activity;   // "running", "idle", "stalled", "unknown"
    };

    explicit Outputs(QObject* parent = nullptr);
    ~Outputs() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int currentRow() const;
    QString currentName() const;
    QString currentActivity() const;
    QString currentGuid() const;
    const Output* current() const;

    Q_INVOKABLE void select(int row);
    // By endpoint GUID, any form canonical_endpoint_guid takes. False if it is not a working output.
    bool selectGuid(const std::wstring& endpoint);
    Q_INVOKABLE void refresh();

signals:
    void countChanged();
    void currentChanged();
    void currentActivityChanged();

private:
    void probe();

    std::vector<Output> outputs_;
    std::wstring current_guid_;
    std::unique_ptr<isotone::devices::DeviceWatcher> watcher_;
    QTimer probe_timer_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> probing_{false};
};
