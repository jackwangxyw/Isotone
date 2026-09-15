// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "equalizerapoconfig.h"

#include <QDir>
#include <QSettings>
#include <QVariantList>

#include "apppaths.h"
#include "config_attach.h"
#include "config_files.h"
#include "devicetoolcontroller.h"
#include "eapo_install.h"

std::filesystem::path equalizerApoConfigDir() {
    const QString moved = AppPaths::compatConfigDir();
    if (!moved.isEmpty()) return moved.toStdWString();
    return isotone::compat::locate_equalizer_apo().config_path;
}

bool equalizerApoAttached() {
    const std::filesystem::path dir = equalizerApoConfigDir();
    return !dir.empty() && isotone::compat::inspect_config(dir).isotone_included;
}

namespace {

// The same file AppSettings keeps; QSettings objects on one file share its data.
QSettings settings_file() {
    return QSettings(QDir(AppPaths::dataDir()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
}

QString off_key(const QString& guid) { return QStringLiteral("outputs/off/") + guid.toLower(); }

}  // namespace

bool equalizerApoOutputOff(const QString& guid) { return settings_file().value(off_key(guid), false).toBool(); }

void setEqualizerApoOutputOff(const QString& guid, bool off) {
    QSettings s = settings_file();
    if (off)
        s.setValue(off_key(guid), true);
    else
        s.remove(off_key(guid));
}

bool equalizerApoOutputListed(const QString& guid, bool attached) { return attached && !equalizerApoOutputOff(guid); }

bool fakeDevicetool() { return qEnvironmentVariableIsSet("ISOTONE_FAKE_DEVICETOOL"); }

QString fakeDevicetoolRefusal() {
    if (!fakeDevicetool() || !AppPaths::compatConfigDir().isEmpty()) return {};
    return QStringLiteral("--fake-devicetool needs --compat-dir: without it Equalizer APO's own config.txt and Isotone.txt would be written");
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
