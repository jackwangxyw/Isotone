// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "equalizerapoconfig.h"

#include <QVariantList>

#include "apppaths.h"
#include "config_attach.h"
#include "devicetoolcontroller.h"
#include "eapo_install.h"

std::filesystem::path equalizerApoConfigDir() {
    const QString moved = AppPaths::compatConfigDir();
    if (!moved.isEmpty()) return moved.toStdWString();
    return isotone::compat::locate_equalizer_apo().config_path;
}

QVariantMap EqualizerApoConfig::preview() const {
    const std::filesystem::path dir = equalizerApoConfigDir();
    QVariantMap out{{QStringLiteral("directory"), QString::fromStdWString(dir.wstring())}};
    if (dir.empty()) {
        out.insert(QStringLiteral("error"), windowsMessage(ERROR_PATH_NOT_FOUND));
        return out;
    }
    const isotone::ui::AttachPreview p = isotone::ui::preview_attach(dir);
    out.insert(QStringLiteral("error"), p.error == ERROR_SUCCESS ? QString() : windowsMessage(p.error));
    QVariantList lines, added;
    for (size_t i = 0; i < p.lines.size(); ++i)
        lines << QVariantMap{{QStringLiteral("text"), QString::fromStdString(p.lines[i])}, {QStringLiteral("peace"), bool(p.peace[i])}};
    for (const std::string& a : p.added) added << QString::fromStdString(a);
    out.insert(QStringLiteral("lines"), lines);
    out.insert(QStringLiteral("added"), added);
    out.insert(QStringLiteral("attached"), p.attached);
    return out;
}

QString EqualizerApoConfig::attach(bool removePeace) {
    const std::filesystem::path dir = equalizerApoConfigDir();
    if (dir.empty()) return windowsMessage(ERROR_PATH_NOT_FOUND);
    const isotone::ui::AttachOutcome o = isotone::ui::attach_config(dir, removePeace);
    if (o.error != ERROR_SUCCESS) return windowsMessage(o.error);
    emit attached();
    return {};
}
