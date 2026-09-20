// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Devicetool on Linux: the QML surface of devicetoolcontroller.h, for the one
// operation Linux has. Nothing is installed per output: a package puts the
// daemon's user unit in place, and Devices can only start it when it is not
// running (kind "start": systemctl --user start isotone-daemon.service). The
// phases are the Windows ones this uses: running, done, failed.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QProcess;

class DevicetoolController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Devicetool)
    QML_SINGLETON

    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString target READ target NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    Q_PROPERTY(bool elevated READ elevated CONSTANT)
    Q_PROPERTY(bool working READ working NOTIFY changed)
    Q_PROPERTY(QVariantMap rowStatus READ rowStatus CONSTANT)
    Q_PROPERTY(bool restarted READ restarted CONSTANT)

public:
    explicit DevicetoolController(QObject* parent = nullptr);
    ~DevicetoolController() override;

    QString phase() const { return phase_; }
    QString kind() const { return kind_; }
    QString target() const { return target_; }
    QString reason() const { return reason_; }
    bool elevated() const { return false; }
    bool working() const { return phase_ == QLatin1String("running"); }
    QVariantMap rowStatus() const { return {}; }
    bool restarted() const { return false; }

    // kind "start" only; anything else is refused as failed.
    Q_INVOKABLE void run(const QString& kind, const QString& guid, const QStringList& args);
    Q_INVOKABLE void requestApproval() {}
    Q_INVOKABLE void apply(const QVariantList&) {}
    Q_INVOKABLE void retry();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void copyDetails();
    Q_INVOKABLE void restartWindows() {}
    Q_INVOKABLE bool loadScript(const QString&) { return false; }

    // The command systemctl is run with; tests replace it.
    void setCommand(const QString& program, const QStringList& args);

signals:
    void changed();
    void finished(const QString& kind, const QString& guid);
    void restartWindowsRequested();
    void outputChoicesChanged();

private:
    QString phase_, kind_, target_, reason_, details_;
    // Set in the constructor when the app is in a Flatpak: the daemon is
    // started directly and detached rather than through systemd.
    bool detached_ = false;
    QString program_ = QStringLiteral("systemctl");
    QStringList args_{QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("isotone-daemon.service")};
    QProcess* process_ = nullptr;
};
