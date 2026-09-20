// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Launch at sign-in for Settings, General: the Run value (startup_registration.h)
// is what the toggle shows, not a copy of it in settings.ini, so it stays true
// when the value is removed elsewhere. The value names this exe, with --tray
// when Start in the tray is on.
//
// The key is HKCU\Software\Microsoft\Windows\CurrentVersion\Run, or
// HKCU\<ISOTONE_RUN_KEY> when that is set: the QML tests and screenshots set it
// to a test key, so they never write the real one.
//
// Linux has the same shape with an XDG autostart entry in place of the value
// (autostart_xdg.h), and ISOTONE_AUTOSTART_DIR in place of ISOTONE_RUN_KEY. In
// a Flatpak the entry is still what the toggle shows, but it is written by the
// Background portal rather than here (startup_portal.h).

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class Startup : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool launchAtSignIn READ launchAtSignIn NOTIFY changed)

public:
    explicit Startup(QObject* parent = nullptr);

    bool launchAtSignIn() const;
    // Writes or removes the value; false when the registry refused.
    Q_INVOKABLE bool setLaunchAtSignIn(bool on, bool tray);
    // Rewrites the value for Start in the tray, when there is one.
    Q_INVOKABLE bool setStartInTray(bool tray);
    // The value as it is, empty when there is none.
    Q_INVOKABLE QString command() const;

    static QString runKey();

signals:
    void changed();
};
