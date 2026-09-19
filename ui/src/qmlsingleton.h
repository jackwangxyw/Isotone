// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// One of the Isotone module's singletons from C++. Qt 6.5 looks a singleton up
// by module and name; 6.4, which Ubuntu 24.04 and Linux Mint 22 ship, only by
// type id, so the id is asked for first there.

#pragma once

#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QtQml/qqml.h>

template <typename T>
T* isotoneSingleton(QQmlEngine* engine, const char* name) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return engine->singletonInstance<T*>("Isotone", name);
#else
    const int id = qmlTypeId("Isotone", 1, 0, name);
    return id < 0 ? nullptr : engine->singletonInstance<T*>(id);
#endif
}

// A singleton defined in QML (UiState). 6.4 gives those no type id, so it is
// read through `root`'s own imports instead.
inline QObject* isotoneQmlSingleton(QQmlEngine* engine, QObject* root, const char* name) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    Q_UNUSED(root);
    return engine->singletonInstance<QObject*>("Isotone", name);
#else
    Q_UNUSED(engine);
    QQmlExpression expression(qmlContext(root), root, QString::fromLatin1(name));
    return expression.evaluate().value<QObject*>();
#endif
}
