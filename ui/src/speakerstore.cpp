// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "speakerstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include "apppaths.h"

namespace {

QJsonObject read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

}  // namespace

SpeakerStore::SpeakerStore(QString path)
    : path_(path.isEmpty() ? QDir(AppPaths::dataDir()).filePath(QStringLiteral("speakers.json")) : std::move(path)) {}

QJsonObject SpeakerStore::output(const std::wstring& guid) const {
    return read_file(path_).value(QStringLiteral("outputs")).toObject().value(QString::fromStdWString(guid)).toObject();
}

bool SpeakerStore::setOutput(const std::wstring& guid, const QJsonObject& value) {
    QJsonObject root = read_file(path_);
    QJsonObject outputs = root.value(QStringLiteral("outputs")).toObject();
    outputs.insert(QString::fromStdWString(guid), value);
    root.insert(QStringLiteral("outputs"), outputs);
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(root).toJson());
    return file.commit();
}

std::vector<isotone::ui::SpeakerGroup> SpeakerStore::groups(const std::wstring& guid) const {
    std::vector<isotone::ui::SpeakerGroup> out;
    for (const QJsonValue& g : output(guid).value(QStringLiteral("groups")).toArray()) {
        isotone::ui::SpeakerGroup group;
        group.name = g.toObject().value(QStringLiteral("name")).toString().toStdString();
        for (const QJsonValue& code : g.toObject().value(QStringLiteral("speakers")).toArray())
            group.codes.push_back(code.toString().toStdString());
        if (!group.name.empty()) out.push_back(std::move(group));
    }
    return out;
}

bool SpeakerStore::setGroups(const std::wstring& guid, const std::vector<isotone::ui::SpeakerGroup>& groups) {
    QJsonArray array;
    for (const isotone::ui::SpeakerGroup& g : groups) {
        QJsonArray codes;
        for (const std::string& c : g.codes) codes.append(QString::fromStdString(c));
        array.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(g.name)}, {QStringLiteral("speakers"), codes}});
    }
    QJsonObject o = output(guid);
    o.insert(QStringLiteral("groups"), array);
    return setOutput(guid, o);
}

double SpeakerStore::farthest(const std::wstring& guid) const {
    const QJsonValue v = output(guid).value(QStringLiteral("farthestM"));
    return v.isDouble() ? v.toDouble() : isotone::ui::kDefaultFarthestM;
}

bool SpeakerStore::setFarthest(const std::wstring& guid, double metres) {
    QJsonObject o = output(guid);
    o.insert(QStringLiteral("farthestM"), metres);
    return setOutput(guid, o);
}
