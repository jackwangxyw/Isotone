// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Presets on disk (plan 7.6): one JSON file per preset in <dataDir>/presets,
// named by an id that never changes, and <dataDir>/outputs.json, which preset
// each output (braced GUID) is assigned. Every write is atomic (a temporary file,
// then a replacing rename). A file that does not read as a preset is skipped.
//
// A preset is the EQ part of a state: bands, preamp, Auto, and the layout the
// band channels address. The rest of a state belongs to the output.

#pragma once

#include <QByteArray>
#include <QString>

#include <map>
#include <vector>

#include "isotone/types.h"

namespace presetfile {

// The schema this build writes, and the newest it reads.
inline constexpr int kVersion = 1;

// {"format": "isotone-preset", "version": 1, "name", "preampDb", "autoPreamp",
//  "forOutput", "layout": {"channels", "speakerMask"}, "bands": [{"id", "type",
//  "fc", "gainDb", "width", "widthMode", "shelfCorner", "channels", "enabled"}]}
//
// "forOutput" is the output the preset is for, a braced GUID; absent or empty is
// every output, which is what a preset is unless it is narrowed (owner,
// 2026-09-15). A file written before it is read as being for every output.
QByteArray write(const QString& name, const isotone::EqState& eq, const QString& for_output = QString());
// False, with the outputs untouched, for anything that is not a whole preset of a
// version this build reads. `for_output` may be null.
bool read(const QByteArray& bytes, QString* name, isotone::EqState* eq, QString* for_output = nullptr);

}  // namespace presetfile

// The EQ part of `state`, for `channels` and `speaker_mask`.
isotone::EqState eq_part(const isotone::EqState& state, uint32_t channels, uint32_t speaker_mask);

// Whether two EQ parts sound the same once `b` is moved to `a`'s layout: bands
// in order (ids aside) within what float32 and a text file keep, the Auto mode,
// and the preamp unless both are in Auto.
bool same_eq(const isotone::EqState& a, const isotone::EqState& b);

class PresetStore {
public:
    struct Preset {
        QString id;          // the file name without .json
        QString name;
        isotone::EqState eq;
        QString for_output;  // braced GUID; empty is every output
    };

    explicit PresetStore(const QString& data_dir);

    // Reads every preset and the assignments again.
    void reload();

    // Sorted by name, case-insensitively, numbers by value.
    const std::vector<Preset>& presets() const { return presets_; }
    const Preset* byName(const QString& name) const;
    const Preset* byId(const QString& id) const;

    // `base` when no other preset than `except_id` has it (trimmed), else base
    // with " 2", " 3", ... in place of a number it ends with.
    QString uniqueName(const QString& base, const QString& except_id = QString()) const;

    // A new preset under a unique name; its id, or empty when it could not be
    // written. `for_output` empty is every output.
    QString add(const QString& name, const isotone::EqState& eq, const QString& for_output = QString());
    // The output a preset is for; empty is every output.
    bool setForOutput(const QString& id, const QString& for_output);
    bool update(const QString& id, const isotone::EqState& eq);
    // The name it now has (unique), or empty when it could not be written.
    QString rename(const QString& id, const QString& name);
    // Removes the file and every assignment to it. False, with nothing changed,
    // when there is no such preset or its file could not be removed.
    bool remove(const QString& id);

    // The preset id assigned to an output, or empty.
    QString assignment(const QString& guid) const;
    // The output's name as it was last seen.
    QString outputName(const QString& guid) const;
    // Empty `id` removes the assignment.
    bool assign(const QString& guid, const QString& id, const QString& output_name);
    bool setOutputName(const QString& guid, const QString& output_name);
    // Outputs assigned `id`, by GUID.
    std::vector<QString> assignedTo(const QString& id) const;

    QString presetsDir() const;

private:
    void sort();
    bool writeOutputs() const;

    QString data_dir_;
    std::vector<Preset> presets_;
    struct Assignment {
        QString preset;
        QString name;
    };
    std::map<QString, Assignment> outputs_;
};
