// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "appsettings.h"

#include <QDir>

#include "apppaths.h"

AppSettings::AppSettings(QObject* parent)
    : QObject(parent),
      settings_(std::make_unique<QSettings>(QDir(AppPaths::dataDir()).filePath(QStringLiteral("settings.ini")),
                                            QSettings::IniFormat)) {
    connect(this, &AppSettings::themeChanged, this, [this] { ++theme_revision_; });
}

AppSettings::~AppSettings() = default;

QVariant AppSettings::value(const QString& key, const QVariant& fallback) const { return settings_->value(key, fallback); }

void AppSettings::setValue(const QString& key, const QVariant& value) {
    if (!write(key, value)) return;
    emit valueChanged(key);
    if (key.startsWith(QLatin1String("appearance/"))) emit themeChanged();
}

bool AppSettings::write(const QString& key, const QVariant& value) {
    if (settings_->contains(key) && settings_->value(key) == value) return false;
    settings_->setValue(key, value);
    return true;
}

// "system" everywhere but a Flatpak, where it cannot mean anything: the sandbox
// cannot read the desktop's setting and the portal reports no preference, so
// Theme.qml's "not explicitly Light" reads as dark whatever the desktop is. The
// app came up dark on a Cinnamon desktop that was light, minutes after the .deb
// build of it came up light (2026-09-20). Rather than offer a choice that does
// nothing, the Flatpak defaults to dark and does not offer System at all
// (owner, 2026-09-20).
bool AppSettings::sandboxed() { return !qEnvironmentVariableIsEmpty("FLATPAK_ID"); }

QString AppSettings::theme() const {
    const QString fallback = sandboxed() ? QStringLiteral("dark") : QStringLiteral("system");
    const QString chosen = settings_->value(QStringLiteral("appearance/theme"), fallback).toString();
    // A settings file carried in from a host install can still say "system".
    return sandboxed() && chosen == QLatin1String("system") ? QStringLiteral("dark") : chosen;
}
void AppSettings::setTheme(const QString& v) {
    if (write(QStringLiteral("appearance/theme"), v)) emit themeChanged();
}
int AppSettings::accent() const { return settings_->value(QStringLiteral("appearance/accent"), 0).toInt(); }
void AppSettings::setAccent(int v) {
    if (write(QStringLiteral("appearance/accent"), v)) emit themeChanged();
}
QString AppSettings::customAccent() const {
    return settings_->value(QStringLiteral("appearance/customAccent"), QStringLiteral("#2f8cff")).toString();
}
void AppSettings::setCustomAccent(const QString& v) {
    if (write(QStringLiteral("appearance/customAccent"), v)) emit themeChanged();
}
QString AppSettings::bandColours() const {
    return settings_->value(QStringLiteral("appearance/bandColours"), QStringLiteral("band")).toString();
}
void AppSettings::setBandColours(const QString& v) {
    if (write(QStringLiteral("appearance/bandColours"), v)) emit themeChanged();
}
bool AppSettings::sidebarOpen() const { return settings_->value(QStringLiteral("window/sidebarOpen"), true).toBool(); }
void AppSettings::setSidebarOpen(bool v) {
    if (write(QStringLiteral("window/sidebarOpen"), v)) emit shellChanged();
}
bool AppSettings::panelOpen() const { return settings_->value(QStringLiteral("window/panelOpen"), true).toBool(); }
void AppSettings::setPanelOpen(bool v) {
    if (write(QStringLiteral("window/panelOpen"), v)) emit shellChanged();
}
bool AppSettings::spectrumOn() const { return settings_->value(QStringLiteral("window/spectrumOn"), true).toBool(); }
void AppSettings::setSpectrumOn(bool v) {
    if (write(QStringLiteral("window/spectrumOn"), v)) emit shellChanged();
}
