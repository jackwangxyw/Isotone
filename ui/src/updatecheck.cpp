// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "updatecheck.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent) {}

QString UpdateCheck::current() { return QStringLiteral(ISOTONE_VERSION); }

std::optional<std::array<int, 3>> UpdateCheck::parseVersion(const QString& text) {
    static const QRegularExpression re(QStringLiteral("^v?(\\d{1,6})\\.(\\d{1,6})\\.(\\d{1,6})$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return std::nullopt;
    return std::array<int, 3>{m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()};
}

QString UpdateCheck::newerTag(const QByteArray& json, const QString& current) {
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()) return {};
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    const auto theirs = parseVersion(tag);
    const auto ours = parseVersion(current);
    if (!theirs || !ours || !(*ours < *theirs)) return {};
    return tag;
}

void UpdateCheck::check(const QString& url) {
    if (running_) return;
    running_ = true;
    if (!network_) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(url.isEmpty() ? QString::fromLatin1(kLatestUrl) : url));
    // GitHub refuses a request with no User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Isotone/%1").arg(current()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(15000);
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        running_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("update check: %s", qPrintable(reply->errorString()));
        } else if (const QString tag = newerTag(reply->readAll(), current()); !tag.isEmpty()) {
            latest_ = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
            // Built here rather than taken from the reply, so the notice only
            // ever opens this repository's releases.
            release_url_ = QStringLiteral("https://github.com/jackwangxyw/Isotone/releases/tag/%1").arg(tag);
            emit changed();
        }
        emit finished();
    });
}
