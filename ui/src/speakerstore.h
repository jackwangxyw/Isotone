// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// What the speaker controls keep per output that the engine state does not
// carry: the user's speaker groups (by speaker code, so a group keeps its
// speakers across layouts) and the farthest speaker's distance, from which the
// distances are read back out of the delays (speaker_setup.h). One JSON file in
// the data directory, speakers.json:
//   {"outputs": {"{guid}": {"farthestM": 3.4,
//                           "groups": [{"name": "Heights", "speakers": ["SL", "SR"]}]}}}

#pragma once

#include <QJsonObject>
#include <QString>

#include <string>
#include <vector>

#include "speaker_setup.h"

class SpeakerStore {
public:
    // speakers.json in AppPaths::dataDir() unless given.
    explicit SpeakerStore(QString path = QString());

    std::vector<isotone::ui::SpeakerGroup> groups(const std::string& guid) const;
    bool setGroups(const std::string& guid, const std::vector<isotone::ui::SpeakerGroup>& groups);
    // kDefaultFarthestM when none is kept.
    double farthest(const std::string& guid) const;
    bool setFarthest(const std::string& guid, double metres);

    const QString& path() const { return path_; }

private:
    QJsonObject output(const std::string& guid) const;
    bool setOutput(const std::string& guid, const QJsonObject& value);

    QString path_;
};
