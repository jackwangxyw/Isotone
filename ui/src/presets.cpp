// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// FOUNDATION STUB, replaced by the presets work package.

#include "presets.h"

Presets::Presets(QObject* parent) : QAbstractListModel(parent) {}

int Presets::rowCount(const QModelIndex&) const { return 0; }
QVariant Presets::data(const QModelIndex&, int) const { return {}; }
QStringList Presets::names() const { return {}; }
QString Presets::currentName() const { return QStringLiteral("Untitled"); }
bool Presets::modified() const { return false; }
void Presets::next() {}
void Presets::previous() {}
void Presets::load(const QString&) {}
void Presets::save() {}
QString Presets::assignedName(const QString&) const { return {}; }
void Presets::assign(const QString&, const QString&) {}
