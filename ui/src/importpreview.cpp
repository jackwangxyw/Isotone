// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "importpreview.h"

#include <QFileInfo>
#include <QStringList>
#include <QVariantMap>

#include <set>

#include "isotone/apo_config.h"

ImportPreview::ImportPreview(const QString& file_name, std::string text, std::vector<isotone::ui::OutputTarget> outputs,
                             const std::wstring& guid, QObject* parent)
    : QObject(parent), file_name_(file_name), text_(std::move(text)), outputs_(std::move(outputs)) {
    for (const isotone::ui::OutputTarget& t : outputs_)
        if (t.guid == guid) target_ = t;
    parse();
}

QString ImportPreview::suggestedName() const { return QFileInfo(file_name_).completeBaseName().trimmed(); }

void ImportPreview::setOutputGuid(const QString& guid) {
    const std::wstring g = guid.toStdWString();
    if (g == target_.guid) return;
    for (const isotone::ui::OutputTarget& t : outputs_) {
        if (t.guid != g) continue;
        target_ = t;
        parse();
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

    // The lines as the file has them, once each.
    const QStringList lines = QString::fromUtf8(text_.data(), static_cast<qsizetype>(text_.size())).split(QLatin1Char('\n'));
    std::set<size_t> seen;
    skipped_.clear();
    for (const isotone::ApoParseMessage& w : result.warnings) {
        if (w.line == 0 || w.line > static_cast<size_t>(lines.size()) || !seen.insert(w.line).second) continue;
        QString line = lines[static_cast<qsizetype>(w.line - 1)];
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        skipped_.append(QVariantMap{{QStringLiteral("line"), static_cast<int>(w.line)}, {QStringLiteral("text"), line.trimmed()}});
    }
}
