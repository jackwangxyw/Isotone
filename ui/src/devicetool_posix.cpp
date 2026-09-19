// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicetool_posix.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>

DevicetoolController::DevicetoolController(QObject* parent) : QObject(parent) {}

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
