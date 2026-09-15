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

QString AppSettings::theme() const { return settings_->value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString(); }
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
