// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "apppaths.h"

#include <QDir>

namespace {

QString& data_override() {
    static QString dir = qEnvironmentVariable("ISOTONE_DATA_DIR");
    return dir;
}

QString& compat_override() {
    static QString dir = qEnvironmentVariable("ISOTONE_COMPAT_DIR");
    return dir;
}

}  // namespace

namespace AppPaths {

QString dataDir() {
    QString dir = data_override();
#if defined(_WIN32)
    // The roaming profile, as the plan puts presets (7.6): %APPDATA%\Isotone.
    if (dir.isEmpty()) dir = QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("Isotone"));
#else
    // $XDG_CONFIG_HOME/isotone/ui, beside the daemon's devices directory; a
    // relative XDG_CONFIG_HOME is ignored, as the spec says.
    if (dir.isEmpty()) {
        QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
        if (config.isEmpty() || QDir::isRelativePath(config)) config = QDir::home().filePath(QStringLiteral(".config"));
        dir = QDir(config).filePath(QStringLiteral("isotone/ui"));
    }
#endif
    QDir().mkpath(dir);
    return dir;
}

void setDataDir(const QString& dir) { data_override() = dir; }

QString compatConfigDir() { return compat_override(); }

void setCompatConfigDir(const QString& dir) { compat_override() = dir; }

}  // namespace AppPaths
