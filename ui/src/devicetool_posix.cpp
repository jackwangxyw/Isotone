// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicetool_posix.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>

DevicetoolController::DevicetoolController(QObject* parent) : QObject(parent) {
    // In a Flatpak there is no systemd to talk to: `systemctl --user` inside a
    // sandbox reaches the sandbox, not the session, and a Flatpak cannot put a
    // unit where the host's systemd would find one. The daemon ships in the
    // same Flatpak, so it is started directly instead.
    //
    // Detached, so it outlives the window: a child of the app would go when the
    // app is closed to the tray or quit, and the EQ would go with it. The
    // sandbox instance stays up while any process in it runs.
    //
    // It still finds the app's region: both are in the host's /dev/shm, which
    // is what --device=shm is for (linux/packaging/flatpak, and plan section 12
    // question 5).
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID")) {
        program_ = QStringLiteral("isotone-daemon");
        args_.clear();
        detached_ = true;
    }
}

DevicetoolController::~DevicetoolController() {
    if (process_) {
        process_->disconnect(this);
        process_->waitForFinished(2000);
    }
}

void DevicetoolController::setCommand(const QString& program, const QStringList& args) {
    program_ = program;
    args_ = args;
}

void DevicetoolController::run(const QString& kind, const QString& guid, const QStringList&) {
    if (working()) return;
    kind_ = kind;
    target_ = guid;
    reason_.clear();
    details_.clear();
    if (kind != QLatin1String("start")) {
        phase_ = QStringLiteral("failed");
        reason_ = QStringLiteral("Not available on Linux.");
        emit changed();
        return;
    }
    phase_ = QStringLiteral("running");
    emit changed();

    if (detached_) {
        qint64 pid = 0;
        const bool started = QProcess::startDetached(program_, args_, QString(), &pid);
        details_ = QStringLiteral("%1 %2\n%3")
                       .arg(program_, args_.join(QLatin1Char(' ')),
                            started ? QStringLiteral("started, pid %1").arg(pid)
                                    : QStringLiteral("could not be started"));
        phase_ = started ? QStringLiteral("done") : QStringLiteral("failed");
        if (!started) reason_ = QStringLiteral("%1 could not be run.").arg(program_);
        emit changed();
        emit finished(kind_, target_);
        return;
    }

    delete process_;
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(process_->readAll()).trimmed();
        details_ = QStringLiteral("%1 %2\nexit %3\n%4").arg(program_, args_.join(QLatin1Char(' '))).arg(code).arg(output);
        const bool ok = status == QProcess::NormalExit && code == 0;
        phase_ = ok ? QStringLiteral("done") : QStringLiteral("failed");
        // systemctl's own line, as the reason: "Unit isotone-daemon.service not found."
        if (!ok) {
            reason_ = output.section(QLatin1Char('\n'), 0, 0);
            if (reason_.isEmpty()) reason_ = QStringLiteral("systemctl exited with %1.").arg(code);
        }
        emit changed();
        emit finished(kind_, target_);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        phase_ = QStringLiteral("failed");
        reason_ = QStringLiteral("%1 could not be run.").arg(program_);
        details_ = reason_;
        emit changed();
        emit finished(kind_, target_);
    });
    process_->start(program_, args_);
}

void DevicetoolController::retry() {
    if (phase_ == QLatin1String("failed")) run(kind_, target_, {});
}

void DevicetoolController::clear() {
    if (working()) return;
    phase_.clear();
    kind_.clear();
    target_.clear();
    reason_.clear();
    emit changed();
}

void DevicetoolController::copyDetails() { QGuiApplication::clipboard()->setText(details_); }
