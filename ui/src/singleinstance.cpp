// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "singleinstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

#if defined(_WIN32)
#include <windows.h>
#endif

SingleInstance::SingleInstance(const QString& name, QObject* parent) : QObject(parent), name_(name) {}

QString SingleInstance::nameFor(const QString& dataDir) {
#if defined(_WIN32)
    const QString key = qEnvironmentVariable("USERNAME") + QLatin1Char('|') + QDir(dataDir).absolutePath().toLower();
#else
    // Paths are case-sensitive here; the socket lives in the user's own runtime directory anyway.
    const QString key = qEnvironmentVariable("USER") + QLatin1Char('|') + QDir(dataDir).absolutePath();
#endif
    return QStringLiteral("Isotone-") + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

bool SingleInstance::notifyRunning(bool show) {
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (!socket.waitForConnected(1000)) return false;
    // The running instance may take the foreground from this launch.
#if defined(_WIN32)
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    socket.write(show ? "show\n" : "tray\n");
    socket.waitForBytesWritten(1000);
    // Closing before the running instance has read the line loses it: wait for its answer.
    const bool answered = socket.waitForReadyRead(3000) && socket.readLine().trimmed() == "ok";
    socket.disconnectFromServer();
    return answered;
}

bool SingleInstance::listen() {
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name_)) return false;
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            const auto read = [this, socket] {
                while (socket->canReadLine()) {
                    const QByteArray line = socket->readLine().trimmed();
                    socket->write("ok\n");
                    socket->flush();
                    if (line == "show") emit showRequested();
                }
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            read();
        }
    });
    return true;
}
