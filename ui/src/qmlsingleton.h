// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// One of the Isotone module's singletons from C++. Qt 6.5 looks a singleton up
// by module and name; 6.4, which Ubuntu 24.04 and Linux Mint 22 ship, only by
// type id, so the id is asked for first there.

#pragma once

#include <QQmlEngine>
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
