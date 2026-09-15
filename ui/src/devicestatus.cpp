// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicestatus.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <mmdeviceapi.h>

using isotone::devices::Backend;
using isotone::devices::IsoApoState;

namespace {

// The prototype's empty value.
const QString kDash = QString(QChar(0x2014));

bool parse_state(const QString& name, IsoApoState* out) {
    static const std::pair<const char*, IsoApoState> names[] = {
        {"interrupted", IsoApoState::interrupted},
        {"unrecorded", IsoApoState::unrecorded},
        {"alongside_equalizerapo", IsoApoState::alongside_equalizerapo},
        {"installed", IsoApoState::installed},
        {"not_installed", IsoApoState::not_installed},
        {"replaced_by_equalizerapo", IsoApoState::replaced_by_equalizerapo},
        {"detached", IsoApoState::detached},
    };
    for (const auto& [n, s] : names) {
        if (name == QLatin1String(n)) {
            *out = s;
            return true;
        }
    }
    return false;
}

bool parse_backend(const QString& name, Backend* out) {
    static const std::pair<const char*, Backend> names[] = {
        {"none", Backend::none}, {"native", Backend::native}, {"equalizerapo", Backend::equalizerapo}, {"conflict", Backend::conflict}};
    for (const auto& [n, b] : names) {
        if (name == QLatin1String(n)) {
            *out = b;
            return true;
        }
    }
    return false;
}

// "Speakers (Realtek(R) Audio)": the connection, then the adapter, as Outputs names them.
QString output_name(const QString& connection, const QString& adapter) {
    return adapter.isEmpty() ? connection : QStringLiteral("%1 (%2)").arg(connection, adapter);
}

QString post_slot_of_mode(const QString& mode) {
    if (mode == QLatin1String("SFX_MFX")) return QStringLiteral("MFX");
    if (mode == QLatin1String("SFX_EFX")) return QStringLiteral("EFX");
    if (mode == QLatin1String("LFX_GFX")) return QStringLiteral("GFX");
    return {};
}

bool isoapo_on(const DeviceFacts& f) {
    return f.backend == Backend::native || f.backend == Backend::conflict;
}

}  // namespace

bool factsFromStatusJson(const QByteArray& json, DeviceFacts* out) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("command")).toString() != QLatin1String("status")) return false;
    DeviceFacts f;
    f.guid = o.value(QStringLiteral("guid")).toString();
    const QJsonObject iso = o.value(QStringLiteral("isoapo")).toObject();
    if (!parse_state(iso.value(QStringLiteral("state")).toString(), &f.isoapo)) return false;
    if (!parse_backend(o.value(QStringLiteral("backend")).toString(), &f.backend)) return false;

    const QJsonValue device = o.value(QStringLiteral("device"));
    if (device.isObject()) {
        const QJsonObject d = device.toObject();
        f.name = output_name(d.value(QStringLiteral("connection")).toString(), d.value(QStringLiteral("name")).toString());
        f.present = !d.value(QStringLiteral("disabled")).toBool() && !d.value(QStringLiteral("unplugged")).toBool();
        f.default_console = d.value(QStringLiteral("default_device")).toBool();
        f.channels = d.value(QStringLiteral("channels")).toInt();
        f.sample_rate = d.value(QStringLiteral("sample_rate")).toInt();
        f.enhancements_disabled = d.value(QStringLiteral("enhancements_disabled")).toBool();
    } else {
        f.present = false;   // not present
    }

    for (const QJsonValue& r : iso.value(QStringLiteral("remedies")).toArray()) f.remedies << r.toString();
    QStringList isoapo_slots, eapo_slots;
    for (const QJsonValue& v : o.value(QStringLiteral("effect_slots")).toArray()) {
        const QJsonObject s = v.toObject();
        const QString kind = s.value(QStringLiteral("kind")).toString();
        if (kind.startsWith(QLatin1String("isoapo"))) isoapo_slots << s.value(QStringLiteral("slot")).toString();
        if (kind.startsWith(QLatin1String("equalizerapo"))) eapo_slots << s.value(QStringLiteral("slot")).toString();
    }
    f.install_mode = iso.value(QStringLiteral("install_mode")).toObject().value(QStringLiteral("effective")).toString();
    // A detached IsoAPO is in no slot: the slot its record says, unless Equalizer APO holds the output.
    if (isoapo_slots.isEmpty() && eapo_slots.isEmpty() && f.isoapo == IsoApoState::detached &&
        !post_slot_of_mode(f.install_mode).isEmpty())
        isoapo_slots << post_slot_of_mode(f.install_mode);
    f.effect_slots = (isoapo_slots + eapo_slots).join(QStringLiteral(" + "));
    f.default_mode = o.value(QStringLiteral("default_install_mode")).toString();
    *out = f;
    return true;
}

DeviceFacts factsFromEndpoint(const isotone::devices::Endpoint& e) {
    DeviceFacts f;
    f.guid = QString::fromStdWString(e.guid);
    f.name = output_name(QString::fromStdWString(e.connection_name.empty() ? e.friendly_name : e.connection_name),
                         QString::fromStdWString(e.device_name));
    f.present = e.state == DEVICE_STATE_ACTIVE;
    f.default_console = e.default_console;
    if (e.format.present) {
        f.channels = e.format.channels;
        f.sample_rate = static_cast<int>(e.format.sample_rate);
    }
    f.isoapo = e.engine.isoapo_state;
    f.backend = e.engine.backend;
    f.enhancements_disabled = e.engine.enhancements_disabled;
    return f;
}

QString statusKey(const DeviceFacts& f) {
    if (!f.present) return QStringLiteral("unplugged");
    switch (f.isoapo) {
        case IsoApoState::interrupted: return QStringLiteral("interrupted");
        case IsoApoState::unrecorded: return QStringLiteral("unrecorded");
        case IsoApoState::alongside_equalizerapo: return QStringLiteral("conflict");
        case IsoApoState::installed:
            return f.enhancements_disabled ? QStringLiteral("enhancements_off") : QStringLiteral("installed");
        case IsoApoState::replaced_by_equalizerapo: return QStringLiteral("replaced");
        case IsoApoState::detached: return QStringLiteral("detached");
        case IsoApoState::not_installed: break;
    }
    return f.backend == Backend::equalizerapo ? QStringLiteral("active") : QStringLiteral("not_installed");
}

QString statusLabel(const QString& key) {
    static const QHash<QString, QString> labels = {
        {QStringLiteral("installed"), QStringLiteral("Installed")},
        {QStringLiteral("active"), QStringLiteral("Active")},
        {QStringLiteral("detached"), QStringLiteral("Detached")},
        {QStringLiteral("conflict"), QStringLiteral("Conflict")},
        {QStringLiteral("replaced"), QStringLiteral("Replaced by Equalizer APO")},
        {QStringLiteral("interrupted"), QStringLiteral("Interrupted")},
        {QStringLiteral("unrecorded"), QStringLiteral("Unrecorded")},
        {QStringLiteral("enhancements_off"), QStringLiteral("Enhancements off")},
        {QStringLiteral("not_installed"), QStringLiteral("Not installed")},
        {QStringLiteral("unplugged"), QStringLiteral("Unplugged")},
    };
    return labels.value(key);
}

QString statusDot(const QString& key) {
    if (key == QLatin1String("installed") || key == QLatin1String("active")) return QStringLiteral("ok");
    if (key == QLatin1String("conflict")) return QStringLiteral("bad");
    if (key == QLatin1String("not_installed") || key == QLatin1String("unplugged")) return QStringLiteral("off");
    return QStringLiteral("warn");
}

bool equalizerApoPresent(const DeviceFacts& f) {
    return f.backend == Backend::equalizerapo || f.backend == Backend::conflict ||
           f.isoapo == IsoApoState::replaced_by_equalizerapo;
}

QString engineColumn(const DeviceFacts& f) {
    switch (f.backend) {
        case Backend::native: return QStringLiteral("Native");
        case Backend::equalizerapo: return QStringLiteral("Equalizer APO");
        case Backend::conflict: return QStringLiteral("Native + Equalizer APO");
        case Backend::none: break;
    }
    // IsoAPO's record without its slot.
    return f.isoapo == IsoApoState::not_installed ? kDash : QStringLiteral("Native");
}

QString engineDetail(const DeviceFacts& f) {
    const QString key = statusKey(f);
    if (key == QLatin1String("not_installed") || key == QLatin1String("unplugged")) return kDash;
    if (f.backend == Backend::conflict) return QStringLiteral("IsoAPO + Equalizer APO");
    if (f.backend == Backend::equalizerapo) return QStringLiteral("Equalizer APO");
    return QStringLiteral("Native (IsoAPO)");
}

QString formatLabel(const DeviceFacts& f) {
    if (f.sample_rate <= 0 || f.channels <= 0) return kDash;
    return QStringLiteral("%1 kHz · %2 ch").arg(QString::number(f.sample_rate / 1000.0, 'g', 4)).arg(f.channels);
}

QString nowEngine(const DeviceFacts& f) {
    if (f.backend == Backend::conflict) return QStringLiteral("IsoAPO + Equalizer APO");
    if (f.backend == Backend::equalizerapo) return QStringLiteral("Equalizer APO");
    if (f.backend == Backend::native || f.isoapo == IsoApoState::detached || f.isoapo == IsoApoState::interrupted)
        return QStringLiteral("IsoAPO");
    return QStringLiteral("Off");
}

bool working(const DeviceFacts& f) {
    const QString key = statusKey(f);
    return key == QLatin1String("installed") || key == QLatin1String("active");
}

QStringList actions(const DeviceFacts& f) {
    const QString key = statusKey(f);
    if (key == QLatin1String("installed")) return {QStringLiteral("test"), QStringLiteral("uninstall")};
    if (key == QLatin1String("active")) return {QStringLiteral("replace"), QStringLiteral("test")};
    if (key == QLatin1String("detached")) {
        if (f.backend == Backend::equalizerapo) return {QStringLiteral("takeBack"), QStringLiteral("keepEapo")};
        return {QStringLiteral("repair"), QStringLiteral("test"), QStringLiteral("uninstall")};
    }
    if (key == QLatin1String("conflict")) return {QStringLiteral("removeEapo"), QStringLiteral("uninstall")};
    if (key == QLatin1String("replaced")) return {QStringLiteral("takeBack"), QStringLiteral("keepEapo")};
    if (key == QLatin1String("interrupted")) return {QStringLiteral("undo")};
    if (key == QLatin1String("unrecorded")) return {QStringLiteral("copyDiagnostics")};
    if (key == QLatin1String("enhancements_off")) return {QStringLiteral("enableEnhancements")};
    if (key == QLatin1String("not_installed")) return {QStringLiteral("install")};
    return {};
}

QVariantMap operation(const DeviceFacts& f, const QString& action) {
    const auto op = [](const char* kind, const QStringList& args) {
        return QVariantMap{{QStringLiteral("kind"), QString::fromLatin1(kind)}, {QStringLiteral("args"), args}};
    };
    const QString& g = f.guid;
    if (action == QLatin1String("install")) return op("install", {QStringLiteral("install"), g});
    if (action == QLatin1String("uninstall") || action == QLatin1String("keepEapo"))
        return op("uninstall", {QStringLiteral("uninstall"), g});
    if (action == QLatin1String("test")) return op("test", {QStringLiteral("test"), g});
    if (action == QLatin1String("takeBack") || action == QLatin1String("removeEapo") ||
        action == QLatin1String("replaceWithIsoApo"))
        return op("replace", {QStringLiteral("install"), g, QStringLiteral("--replace-equalizerapo")});
    if (action == QLatin1String("enableEnhancements")) return op("repair", {QStringLiteral("enable-enhancements"), g});
    if (action == QLatin1String("undo")) return op("repair", {QStringLiteral("repair")});
    if (action == QLatin1String("repair")) {
        // An install record from before Isotone.InstallMode does not say the
        // slot: the mode an install would pick, as `repair --mode` asks.
        const QString slot = post_slot_of_mode(f.default_mode).toLower();
        if (f.remedies.contains(QStringLiteral("repair --mode")) && !slot.isEmpty())
            return op("repair", {QStringLiteral("repair"), QStringLiteral("--mode"), slot});
        return op("repair", {QStringLiteral("repair")});
    }
    return {};
}

QVariantMap planChange(const DeviceFacts& f, const QString& want) {
    const QString now = nowEngine(f);
    const QString& g = f.guid;
    QVariantList commands;
    bool attach = false, remove_block = false;
    QString result;
    if (want == QLatin1String("IsoAPO")) {
        result = QStringLiteral("installed");
        if (now != QLatin1String("IsoAPO")) {
            commands << QVariant(equalizerApoPresent(f)
                                     ? QStringList{QStringLiteral("install"), g, QStringLiteral("--replace-equalizerapo")}
                                     : QStringList{QStringLiteral("install"), g});
        }
    } else if (want == QLatin1String("Equalizer APO")) {
        result = QStringLiteral("attached");
        if (now != QLatin1String("Equalizer APO")) {
            // Uninstalling IsoAPO keeps the Equalizer APO on the output.
            commands << QVariant(QStringList{QStringLiteral("uninstall"), g});
            attach = true;
        }
    } else {
        result = QStringLiteral("removed");
        if (isoapo_on(f) || f.isoapo != IsoApoState::not_installed) commands << QVariant(QStringList{QStringLiteral("uninstall"), g});
        // Equalizer APO stays in the output's slots (only its own uninstaller
        // removes it); Isotone stops writing to it.
        remove_block = equalizerApoPresent(f);
    }
    return {{QStringLiteral("guid"), g},
            {QStringLiteral("commands"), commands},
            {QStringLiteral("attach"), attach},
            {QStringLiteral("removeBlock"), remove_block},
            {QStringLiteral("result"), result},
            {QStringLiteral("changed"), want != now}};
}
