// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// An Equalizer APO file being imported (ImportDialog board): read with
// parse_apo_config for the layout of the output it is for, and read again when
// that output changes (ui-spec.md, "Import parses for the device the preset is
// for"). Presets.openImport makes one.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <string>
#include <vector>

#include "devicelink.h"
#include "isotone/types.h"

class ImportPreview : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made by Presets.openImport")

    Q_PROPERTY(QString fileName READ fileName CONSTANT)
    // The file's name without its extension.
    Q_PROPERTY(QString suggestedName READ suggestedName CONSTANT)
    // The output it is for (braced GUID); empty when there is none.
    Q_PROPERTY(QString outputGuid READ outputGuid WRITE setOutputGuid NOTIFY changed)
    Q_PROPERTY(double preampDb READ preampDb NOTIFY changed)
    Q_PROPERTY(int filterCount READ filterCount NOTIFY changed)
    // [{line, text}]: each line with a warning, as the file has it.
    Q_PROPERTY(QVariantList skipped READ skipped NOTIFY changed)
    // False when no filter imports.
    Q_PROPERTY(bool usable READ usable NOTIFY changed)

public:
    // `outputs`: the outputs it can be for; `guid` the one it is for at first.
    ImportPreview(const QString& file_name, std::string text, std::vector<isotone::ui::OutputTarget> outputs,
                  const std::wstring& guid, QObject* parent = nullptr);

    QString fileName() const { return file_name_; }
    QString suggestedName() const;
    QString outputGuid() const { return QString::fromStdWString(target_.guid); }
    void setOutputGuid(const QString& guid);
    double preampDb() const { return state_.preamp_db; }
    int filterCount() const { return static_cast<int>(state_.bands.size()); }
    QVariantList skipped() const { return skipped_; }
    bool usable() const { return !state_.bands.empty(); }

    const isotone::EqState& state() const { return state_; }
    const isotone::ui::OutputTarget& target() const { return target_; }

signals:
    void changed();

private:
    void parse();

    QString file_name_;
    std::string text_;
    std::vector<isotone::ui::OutputTarget> outputs_;
    isotone::ui::OutputTarget target_;
    isotone::EqState state_;
    QVariantList skipped_;
};
