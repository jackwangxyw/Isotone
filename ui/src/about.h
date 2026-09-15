// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Settings, About: the app's version, the engine rows and protected audio
// (diagnostics.h says where each comes from), and Copy diagnostics. Read on
// refresh(), which the page calls when it opens.

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class About : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString isoapo READ isoapo NOTIFY changed)
    Q_PROPERTY(QString equalizerApo READ equalizerApo NOTIFY changed)
    Q_PROPERTY(bool protectedAudioDisabled READ protectedAudioDisabled NOTIFY changed)

public:
    explicit About(QObject* parent = nullptr);

    static QString version();
    QString isoapo() const { return isoapo_; }
    QString equalizerApo() const { return equalizer_apo_; }
    bool protectedAudioDisabled() const { return protected_audio_disabled_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString diagnostics() const;
    // The diagnostics on the clipboard.
    Q_INVOKABLE void copyDiagnostics() const;

signals:
    void changed();

private:
    QString isoapo_, equalizer_apo_;
    bool protected_audio_disabled_ = false;
};
