// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// An Equalizer APO file being imported (ImportDialog board): read with
// parse_apo_config for the layout of the output it is for, and read again when
// that output changes (ui-spec.md, "Import parses for the device the preset is
// for"). Presets.openImport makes one.
//
// A file that holds a magnitude curve instead of filters (GraphicEQ, or the
// FilterCurve that Audacity, AutoEQ and Peace write) has bands fitted to it
// (curve_import.h), since the engine is biquads (owner, 2026-09-15).

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
    // The output it is for (braced GUID). Empty is "every output": the preset is
    // read for the current output's layout and loaded there, and no other output
    // is written (owner, 2026-09-15; it used to make you pick one).
    Q_PROPERTY(QString outputGuid READ outputGuid WRITE setOutputGuid NOTIFY changed)
    // True while it is for every output.
    Q_PROPERTY(bool forEveryOutput READ forEveryOutput NOTIFY changed)
    Q_PROPERTY(double preampDb READ preampDb NOTIFY changed)
    Q_PROPERTY(int filterCount READ filterCount NOTIFY changed)
    // [{line, text}]: each line with a warning, and each filter past the 64th
    // (kParamMaxBands), as the file has it.
    Q_PROPERTY(QVariantList skipped READ skipped NOTIFY changed)
    // False when no filter imports.
    Q_PROPERTY(bool usable READ usable NOTIFY changed)
    // A curve file: how many points it held (0 for a file of filters), and how far
    // the fitted bands are from it at worst, in dB.
    Q_PROPERTY(int curvePoints READ curvePoints NOTIFY changed)
    Q_PROPERTY(double fitWorstDb READ fitWorstDb NOTIFY changed)

public:
    // `outputs`: the outputs it can be for; `guid` the one it is for at first.
    ImportPreview(const QString& file_name, std::string text, std::vector<isotone::ui::OutputTarget> outputs,
                  const std::string& guid, QObject* parent = nullptr);

    QString fileName() const { return file_name_; }
    QString suggestedName() const;
    QString outputGuid() const { return for_every_output_ ? QString() : QString::fromStdString(target_.guid); }
    bool forEveryOutput() const { return for_every_output_; }
    void setOutputGuid(const QString& guid);
    double preampDb() const { return state_.preamp_db; }
    int filterCount() const { return static_cast<int>(state_.bands.size()); }
    QVariantList skipped() const { return skipped_; }
    bool usable() const { return !state_.bands.empty(); }
    int curvePoints() const { return curve_points_; }
    double fitWorstDb() const { return fit_worst_db_; }

    const isotone::EqState& state() const { return state_; }
    const isotone::ui::OutputTarget& target() const { return target_; }

signals:
    void changed();

private:
    void parse();

    QString file_name_;
    std::string text_;
    std::vector<isotone::ui::OutputTarget> outputs_;
    isotone::ui::OutputTarget target_;   // whose layout it is read for
    bool for_every_output_ = true;
    isotone::EqState state_;
    QVariantList skipped_;
    int curve_points_ = 0;
    double fit_worst_db_ = 0;
};
