// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Presets: saved EQ (bands, preamp, Auto) by name, and which preset each output
// is assigned. A list model of the presets, for the presets popover.
//
// FOUNDATION STUB: the API other screens call (Devices, the tray, shortcuts).
// Its implementation is the presets work package; keep these names.

#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class Presets : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // The names, sorted as the popover lists them.
    Q_PROPERTY(QStringList names READ names NOTIFY countChanged)
    // The current output's preset, and whether its edits are unsaved.
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)

public:
    explicit Presets(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    QStringList names() const;
    QString currentName() const;
    bool modified() const;

    // Loads the next or previous preset (in `names` order) on the current output.
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // Loads `name` on the current output.
    Q_INVOKABLE void load(const QString& name);
    // Saves the current output's edits to its preset.
    Q_INVOKABLE void save();
    // The preset assigned to an output (braced GUID), or empty.
    Q_INVOKABLE QString assignedName(const QString& guid) const;
    Q_INVOKABLE void assign(const QString& guid, const QString& name);

signals:
    void countChanged();
    void currentChanged();
    void modifiedChanged();
};
