// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Check for updates on startup (owner, 2026-09-20): asks GitHub for the latest
// release once, and holds its version when it is newer than this build. The
// notice (UpdateNotice.qml) shows it once the window is open, so a start in the
// tray stays silent until the window is. A check that fails leaves nothing
// found and says why on stderr; there is nothing for the user to do about it.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <optional>

class QNetworkAccessManager;

class UpdateCheck : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The newer version found ("0.2.0"), empty until one is.
    Q_PROPERTY(QString latest READ latest NOTIFY changed)
    Q_PROPERTY(QString current READ current CONSTANT)
    // The release's page on GitHub, built from its tag.
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)

public:
    explicit UpdateCheck(QObject* parent = nullptr);

    static constexpr const char* kLatestUrl = "https://api.github.com/repos/jackwangxyw/Isotone/releases/latest";

    QString latest() const { return latest_; }
    static QString current();
    QString releaseUrl() const { return release_url_; }

    // Asks `url` (empty: GitHub's latest release) once; a second call while one
    // is running does nothing.
    Q_INVOKABLE void check(const QString& url = QString());

    // "0.2.0" or "v0.2.0" as three numbers; nothing for anything else, a
    // pre-release suffix included.
    static std::optional<std::array<int, 3>> parseVersion(const QString& text);
    // The release's tag when the JSON is a release newer than `current`, else empty.
    static QString newerTag(const QByteArray& json, const QString& current);

signals:
    void changed();
    // The check ended, found or not (tests).
    void finished();

private:
    QNetworkAccessManager* network_ = nullptr;
    bool running_ = false;
    QString latest_, release_url_;
};
