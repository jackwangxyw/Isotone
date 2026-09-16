// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Presets: saved EQ (bands, preamp, Auto) by name, and which preset each output
// is assigned. A list model of the presets, for the presets popover.
//
// The current output is EqSession's. Its preset is the one assigned to it; an
// output with none, or after New, is "Untitled" until it is saved under a name.
// `modified` compares what the output plays with its preset (with an unassigned
// output, with what it played when it was first shown), so it survives
// switching outputs and restarting, and an undo back to the saved EQ clears it.
//
// Loading or saving a preset writes the output's saved state as well as what it
// plays. Saving a preset writes it to every other working output assigned it,
// through that output's own DeviceLink; edits that are not saved go nowhere else.

#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "devicelink.h"
#include "presetstore.h"

class EqSession;
class ImportPreview;
class QQmlEngine;
class QJSEngine;

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
    // True while the current output has no preset of its own ("Untitled"): Save
    // needs a name first (saveAs).
    Q_PROPERTY(bool untitled READ untitled NOTIFY currentChanged)

public:
    // AssignedRole: the name of the output the preset is for, empty for every
    // output; ForOutputRole its GUID.
    enum Role { NameRole = Qt::UserRole + 1, AssignedRole, CurrentRole, ForOutputRole };

    struct OutputInfo {
        isotone::ui::OutputTarget target;
        QString name;
    };
    using OutputList = std::function<std::vector<OutputInfo>()>;

    // The app's: presets under AppPaths::dataDir(), the EqSession and Outputs singletons.
    static Presets* create(QQmlEngine* qml, QJSEngine* js);
    // `outputs` lists the working outputs; other outputs are written through
    // DeviceLinks on `region_namespace` and `compat_dir`, as EqSession's.
    Presets(const QString& data_dir, EqSession* session, OutputList outputs, std::wstring region_namespace,
            std::wstring compat_dir, QObject* parent = nullptr);
    ~Presets() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList names() const;
    QString currentName() const;
    bool modified() const { return modified_; }
    bool untitled() const;

    // Loads the next or previous preset (in `names` order) on the current output.
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // Loads `name` on the current output. With unsaved changes it loads nothing
    // and emits unsavedChanges(name); save() or revert() first.
    Q_INVOKABLE void load(const QString& name);
    // Saves the current output's edits to its preset. Does nothing while untitled.
    Q_INVOKABLE void save();
    // The preset assigned to an output (braced GUID), or empty.
    Q_INVOKABLE QString assignedName(const QString& guid) const;
    // Assigns `name` (empty: none) and writes it to the output; on the current
    // output, as load() does.
    Q_INVOKABLE void assign(const QString& guid, const QString& name);

    // The output a preset is for: a braced GUID, or empty for every output. Every
    // preset is listed on every output either way; this is what it is meant for.
    Q_INVOKABLE void setPresetOutput(const QString& name, const QString& guid);
    // The output a preset is for, empty for every output.
    Q_INVOKABLE QString presetOutput(const QString& name) const;
    // The current output's EQ as a new preset, for `forOutput` (empty: every
    // output), assigned to it. The name it was
    // given (unique), or empty.
    Q_INVOKABLE QString saveAs(const QString& name, const QString& forOutput = QString());
    // The name it now has, or empty.
    Q_INVOKABLE QString rename(const QString& name, const QString& to);
    // A copy under a unique name; its name.
    Q_INVOKABLE QString duplicate(const QString& name);
    // Outputs assigned it become untitled, playing what they play.
    Q_INVOKABLE void remove(const QString& name);
    // The current output becomes untitled and flat, with Auto preamp on or off.
    Q_INVOKABLE void newPreset(bool autoPreamp);
    // Don't save: the output back to its preset (untitled: to what it played).
    Q_INVOKABLE void revert();
    Q_INVOKABLE QString uniqueName(const QString& base) const;

    // Import: an Equalizer APO file read for the current output. Null when the
    // file cannot be read. The caller owns it.
    Q_INVOKABLE ImportPreview* openImport(const QUrl& file);
    // Creates the preset and loads it on the output it is for. Its name, or empty.
    Q_INVOKABLE QString importPreset(ImportPreview* preview, const QString& name);
    // Working outputs as {guid, name, current}, the current output first.
    Q_INVOKABLE QVariantList outputChoices() const;

    // Export: layouts for the current output, {label, channels, mask}; none for a
    // stereo output.
    Q_INVOKABLE QVariantList exportLayouts() const;
    // The current output's EQ as Equalizer APO text for a layout (0 channels: the
    // output's own).
    Q_INVOKABLE QString exportText(int channels, int speakerMask) const;
    Q_INVOKABLE bool exportFile(const QUrl& file, int channels, int speakerMask) const;

signals:
    void countChanged();
    void currentChanged();
    void modifiedChanged();
    // load(), next() or previous() found unsaved changes; `name` is what was asked for.
    void unsavedChanges(const QString& name);

private:
    struct OutputMemory {
        isotone::EqState baseline;   // what an unassigned output played when first shown
        bool has_baseline = false;
        bool detached = false;       // after New
    };

    QString currentGuid() const;
    const PresetStore::Preset* currentPreset() const;
    QString outputName(const QString& guid) const;
    // The empty GUID (no output: tests) is assigned in memory only.
    void setAssignment(const QString& guid, const QString& id);
    OutputMemory& memory();
    void outputChanged();
    void refresh();
    void rebuild();
    const PresetStore::Preset* presetAt(int row) const;
    void step(int by);
    // Loads a preset's EQ on the current output: EqSession, the output and its saved state.
    void apply(const PresetStore::Preset& p);
    // Writes `eq` to another output, keeping what is its own.
    void writeTo(const isotone::ui::OutputTarget& target, const isotone::EqState& eq);
    void propagate(const QString& id);

    PresetStore store_;
    QPointer<EqSession> session_;
    OutputList outputs_;
    std::wstring namespace_;
    std::wstring compat_dir_;
    std::map<QString, OutputMemory> memory_;
    QString none_assignment_;
    QString current_name_;
    bool modified_ = false;
};
