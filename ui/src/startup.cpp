// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "startup.h"

#include <QCoreApplication>
#include <QDir>

#if defined(_WIN32)
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
#else
#include <QtGlobal>

#include "appsettings.h"
#include "autostart_xdg.h"
#include "startup_portal.h"

Startup::Startup(QObject* parent) : QObject(parent) {}

// Linux: the directory of the XDG autostart entry, or ISOTONE_AUTOSTART_DIR
// when that is set, as the tests do so they never write the real one.
QString Startup::runKey() {
    const QString override = qEnvironmentVariable("ISOTONE_AUTOSTART_DIR");
    return override.isEmpty() ? QString::fromStdString(isotone::ui::autostart_dir()) : override;
}

namespace {

std::string entry_path() { return isotone::ui::autostart_path(Startup::runKey().toStdString()); }
std::string legacy_path() { return isotone::ui::legacy_autostart_path(Startup::runKey().toStdString()); }

// The entry this version writes, or the one an older version left behind. Both
// start the app; only the first gets it an application ID the GlobalShortcuts
// portal will accept, so the old one is read but never written.
std::string live_path() {
    const std::string current = entry_path();
    if (isotone::ui::autostart_enabled(current)) return current;
    const std::string legacy = legacy_path();
    return isotone::ui::autostart_enabled(legacy) ? legacy : current;
}

}  // namespace

bool Startup::launchAtSignIn() const { return isotone::ui::autostart_enabled(live_path()); }

QString Startup::command() const { return QString::fromStdString(isotone::ui::autostart_command(live_path())); }

bool Startup::setLaunchAtSignIn(bool on, bool tray) {
    // In a Flatpak the entry is the Background portal's to write: the directory
    // the desktop reads is outside the sandbox and mounted read-only, and the
    // one inside is read by nothing (startup_portal.h).
    if (AppSettings::sandboxed()) {
        QStringList command{QStringLiteral("isotone")};
        if (tray) command << QStringLiteral("--tray");
        QString detail;
        const bool accepted = isotone::ui::request_autostart(on, command, &detail);
        qInfo("launch at sign-in %s through the Background portal: %s (%s)", on ? "on" : "off",
              accepted ? "accepted" : "refused", qPrintable(detail));
        emit changed();
        // The entry is the state, so that is what is reported, not the call.
        return accepted && launchAtSignIn() == on;
    }

    const std::string exe = QCoreApplication::applicationFilePath().toStdString();
    const int error = on ? isotone::ui::write_autostart(entry_path(), exe, tray) : isotone::ui::remove_autostart(entry_path());
    // Either way the old name goes: on, it would start the app a second time
    // under an ID that resolves to nothing; off, it would go on starting it at
    // all.
    isotone::ui::remove_autostart(legacy_path());
    emit changed();
    return error == 0;
}

bool Startup::setStartInTray(bool tray) {
    if (!launchAtSignIn()) return true;
    return setLaunchAtSignIn(true, tray);
}
#endif
