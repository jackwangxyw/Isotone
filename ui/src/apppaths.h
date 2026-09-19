// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Where the app keeps its files, and where the Equalizer APO backend writes.
// Both can be moved for checks and tests (--data-dir, --compat-dir, or the
// ISOTONE_DATA_DIR and ISOTONE_COMPAT_DIR environment variables), so nothing a
// check does reaches the owner's settings, presets or Equalizer APO install.

#pragma once

#include <QString>

namespace AppPaths {

// %APPDATA%\Isotone on Windows, $XDG_CONFIG_HOME/isotone/ui on Linux, unless
// moved. Created on first use.
QString dataDir();
void setDataDir(const QString& dir);

// Empty: Equalizer APO's own config directory, from its install.
QString compatConfigDir();
void setCompatConfigDir(const QString& dir);

}  // namespace AppPaths
