// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// One app per user and data directory. The first launch listens on a local
// socket (a named pipe, this user only); a later launch connects, asks it to
// show its window unless it was started with --tray, and exits. Named from the
// user and the data directory, so a --data-dir run for tests or screenshots is
// an instance of its own and never brings the owner's window forward.

#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(const QString& name, QObject* parent = nullptr);

    static QString nameFor(const QString& dataDir);

    // True when an instance is running; it is asked to show its window when `show`.
    bool notifyRunning(bool show);
    // Starts answering later launches. False when the name is taken.
    bool listen();

signals:
    // A later launch asked for the window.
    void showRequested();

private:
    QString name_;
    QLocalServer* server_ = nullptr;
};
