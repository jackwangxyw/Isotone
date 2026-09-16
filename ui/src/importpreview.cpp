// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "importpreview.h"

#include <QFileInfo>
#include <QStringList>
#include <QVariantMap>

#include <set>

#include "isotone/apo_config.h"
#include "isotone/curve_import.h"
#include "isotone/param_block.h"

ImportPreview::ImportPreview(const QString& file_name, std::string text, std::vector<isotone::ui::OutputTarget> outputs,
                             const std::wstring& guid, QObject* parent)
    : QObject(parent), file_name_(file_name), text_(std::move(text)), outputs_(std::move(outputs)) {
    for (const isotone::ui::OutputTarget& t : outputs_)
        if (t.guid == guid) target_ = t;
    parse();
}

QString ImportPreview::suggestedName() const { return QFileInfo(file_name_).completeBaseName().trimmed(); }

void ImportPreview::setOutputGuid(const QString& guid) {
    // Empty: every output. The layout stays the one it started with (the current
    // output's), since a preset has to be read for some layout.
    if (guid.isEmpty()) {
        if (for_every_output_) return;
        for_every_output_ = true;
        emit changed();
        return;
    }
    const std::wstring g = guid.toStdWString();
    if (!for_every_output_ && g == target_.guid) return;
    for (const isotone::ui::OutputTarget& t : outputs_) {
        if (t.guid != g) continue;
        for_every_output_ = false;
        if (t.guid != target_.guid) {
            target_ = t;
            parse();
        }
        emit changed();
        return;
    }
}

void ImportPreview::parse() {
    // Without an output, as the parser reads an unspecified layout: stereo.
    const isotone::ChannelLayout layout = target_.backend == isotone::ui::Backend::none
                                              ? isotone::ChannelLayout{}
                                              : isotone::ChannelLayout{target_.layout.channels, target_.layout.speaker_mask};
    isotone::ApoParseResult result = isotone::parse_apo_config(text_, layout);
    state_ = std::move(result.state);

    // A curve instead of filters: fit bands to it, and keep the file's own lines
    // out of the skipped list, since nothing was skipped.
    const std::vector<isotone::CurvePoint> curve =
        state_.bands.empty() ? isotone::parse_curve(text_) : std::vector<isotone::CurvePoint>{};
    curve_points_ = static_cast<int>(curve.size());
    fit_worst_db_ = 0;
    if (curve_points_ > 1) {
        const isotone::CurveFit fit = isotone::fit_curve(curve);
        if (!fit.bands.empty()) {
            state_.bands = fit.bands;
            fit_worst_db_ = fit.worst_db;
            result.warnings.clear();
        }
    }

    // At most kParamMaxBands filters (engine contract): the first ones; the rest are skipped lines.
    std::set<size_t> skipped_lines;
    for (const isotone::ApoParseMessage& w : result.warnings) skipped_lines.insert(w.line);
    if (state_.bands.size() > isotone::kParamMaxBands) {
        for (size_t i = isotone::kParamMaxBands; i < result.band_lines.size(); ++i) skipped_lines.insert(result.band_lines[i]);
        state_.bands.resize(isotone::kParamMaxBands);
    }

    // The lines as the file has them, once each, in file order.
    const QStringList lines = QString::fromUtf8(text_.data(), static_cast<qsizetype>(text_.size())).split(QLatin1Char('\n'));
    skipped_.clear();
    for (size_t n : skipped_lines) {
        if (n == 0 || n > static_cast<size_t>(lines.size())) continue;
        QString line = lines[static_cast<qsizetype>(n - 1)];
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        skipped_.append(QVariantMap{{QStringLiteral("line"), static_cast<int>(n)}, {QStringLiteral("text"), line.trimmed()}});
    }
}
