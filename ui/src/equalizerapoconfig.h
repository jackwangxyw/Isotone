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
