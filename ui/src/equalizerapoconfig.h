// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Equalizer APO's config directory for the Attach dialog: AppPaths'
// compatConfigDir when moved (--compat-dir, tests), else the install's
// ConfigPath. Attaching needs no elevation: Users have full control of that
// directory (config_attach.h).

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>

// Empty when not moved and Equalizer APO is not installed.
std::filesystem::path equalizerApoConfigDir();

// config.txt there includes Isotone.txt for every device (the compat library's
// inspect_config). Until it does, edits to an Equalizer APO output do nothing.
bool equalizerApoAttached();

// Settings Outputs' Off on an output Equalizer APO stays on: Isotone leaves it
// alone (not in Outputs, its block removed) until Equalizer APO is chosen again.
// settings.ini in AppPaths::dataDir(), outputs/off/<guid>.
bool equalizerApoOutputOff(const QString& guid);
void setEqualizerApoOutputOff(const QString& guid, bool off);
// Outputs lists an Equalizer APO output only while attached and not turned Off.
bool equalizerApoOutputListed(const QString& guid, bool attached);

// --fake-devicetool (ISOTONE_FAKE_DEVICETOOL): devicetool and the Devices outputs
// from a script.
bool fakeDevicetool();
// Why a fake devicetool must not run: without a sandbox config directory
// (--compat-dir), Attach and Settings Outputs would write the installed
// Equalizer APO's config.txt and Isotone.txt. Empty when it may.
QString fakeDevicetoolRefusal();

class EqualizerApoConfig : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit EqualizerApoConfig(QObject* parent = nullptr) : QObject(parent) {}

    // {"directory", "error" (a sentence, empty when read), "lines": [{"text", "peace"}],
    //  "added": [text], "attached": bool}
    Q_INVOKABLE QVariantMap preview() const;
    // Attaches, removing the Peace include first when asked. Empty, or the reason.
    Q_INVOKABLE QString attach(bool removePeace);

signals:
    void attached();
};
