// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Check for updates: which versions count as newer, which releases count at
// all, and the whole request through a file:// URL, which QNetworkAccessManager
// answers the way it answers GitHub.

#include "doctest.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include "updatecheck.h"

namespace {

QByteArray release(const char* tag, bool prerelease = false, bool draft = false) {
    return QStringLiteral(R"({"tag_name": "%1", "prerelease": %2, "draft": %3, "html_url": "https://example.com/elsewhere"})")
        .arg(QLatin1String(tag), QLatin1String(prerelease ? "true" : "false"), QLatin1String(draft ? "true" : "false"))
        .toUtf8();
}

// Runs a check against `json` written to a file, and waits for it to end.
void run(UpdateCheck* check, const QByteArray& json) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QFile f(dir.filePath(QStringLiteral("latest.json")));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(json);
    f.close();
    QSignalSpy finished(check, &UpdateCheck::finished);
    check->check(QUrl::fromLocalFile(f.fileName()).toString());
    REQUIRE(finished.wait(5000));
}

}  // namespace

TEST_CASE("a version is three numbers, with or without a v") {
    CHECK(UpdateCheck::parseVersion(QStringLiteral("0.1.0")) == std::array<int, 3>{0, 1, 0});
    CHECK(UpdateCheck::parseVersion(QStringLiteral("v1.12.3")) == std::array<int, 3>{1, 12, 3});
    for (const char* bad : {"", "v", "1.2", "1.2.3.4", "1.2.3-beta", "v1.2.x", " 1.2.3", "V1.2.3", "1..3"}) {
        CAPTURE(bad);
        CHECK_FALSE(UpdateCheck::parseVersion(QLatin1String(bad)).has_value());
    }
}

TEST_CASE("only a later release is newer, compared as numbers") {
    CHECK(UpdateCheck::newerTag(release("v0.2.0"), QStringLiteral("0.1.0")) == QStringLiteral("v0.2.0"));
    CHECK(UpdateCheck::newerTag(release("v0.1.1"), QStringLiteral("0.1.0")) == QStringLiteral("v0.1.1"));
    CHECK(UpdateCheck::newerTag(release("1.0.0"), QStringLiteral("0.9.9")) == QStringLiteral("1.0.0"));
    // 0.10.0 is after 0.9.0, which a comparison of the text gets wrong.
    CHECK(UpdateCheck::newerTag(release("v0.10.0"), QStringLiteral("0.9.0")) == QStringLiteral("v0.10.0"));
    CHECK(UpdateCheck::newerTag(release("v0.1.0"), QStringLiteral("0.1.0")).isEmpty());
    CHECK(UpdateCheck::newerTag(release("v0.0.9"), QStringLiteral("0.1.0")).isEmpty());
}

TEST_CASE("a pre-release, a draft or a tag that is not a version is not an update") {
    CHECK(UpdateCheck::newerTag(release("v0.2.0", true), QStringLiteral("0.1.0")).isEmpty());
    CHECK(UpdateCheck::newerTag(release("v0.2.0", false, true), QStringLiteral("0.1.0")).isEmpty());
    CHECK(UpdateCheck::newerTag(release("v0.2.0-rc1"), QStringLiteral("0.1.0")).isEmpty());
    CHECK(UpdateCheck::newerTag("not json", QStringLiteral("0.1.0")).isEmpty());
    CHECK(UpdateCheck::newerTag(R"({"message": "Not Found"})", QStringLiteral("0.1.0")).isEmpty());
}

TEST_CASE("a newer release is held with its page, built from the tag") {
    UpdateCheck check;
    QSignalSpy changed(&check, &UpdateCheck::changed);
    run(&check, release("v99.0.0"));
    CHECK(changed.count() == 1);
    CHECK(check.latest() == QStringLiteral("99.0.0"));
    // This repository's page, not the reply's html_url.
    CHECK(check.releaseUrl() == QStringLiteral("https://github.com/jackwangxyw/Isotone/releases/tag/v99.0.0"));
}

TEST_CASE("this version, or a failed request, finds nothing") {
    UpdateCheck check;
    QSignalSpy changed(&check, &UpdateCheck::changed);
    run(&check, release(("v" + UpdateCheck::current().toStdString()).c_str()));
    CHECK(check.latest().isEmpty());

    QSignalSpy finished(&check, &UpdateCheck::finished);
    check.check(QUrl::fromLocalFile(QStringLiteral("C:/no/such/isotone/latest.json")).toString());
    REQUIRE(finished.wait(5000));
    CHECK(check.latest().isEmpty());
    CHECK(changed.count() == 0);
}
