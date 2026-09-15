// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "startup.h"

#include <QCoreApplication>
#include <QDir>

#include "startup_registration.h"

Startup::Startup(QObject* parent) : QObject(parent) {}

QString Startup::runKey() {
    const QString override = qEnvironmentVariable("ISOTONE_RUN_KEY");
    return override.isEmpty() ? QString::fromWCharArray(isotone::ui::kRunKey) : override;
}

bool Startup::launchAtSignIn() const {
    return isotone::ui::read_run_value(runKey().toStdWString(), isotone::ui::kRunValueName).has_value();
}

QString Startup::command() const {
    const auto value = isotone::ui::read_run_value(runKey().toStdWString(), isotone::ui::kRunValueName);
    return value ? QString::fromStdWString(*value) : QString();
}

bool Startup::setLaunchAtSignIn(bool on, bool tray) {
    const std::wstring key = runKey().toStdWString();
    const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const LSTATUS status = on ? isotone::ui::write_run_value(key, isotone::ui::kRunValueName, isotone::ui::run_command(exe, tray))
                              : isotone::ui::remove_run_value(key, isotone::ui::kRunValueName);
    emit changed();
    return status == ERROR_SUCCESS;
}

bool Startup::setStartInTray(bool tray) {
    if (!launchAtSignIn()) return true;
    return setLaunchAtSignIn(true, tray);
}
