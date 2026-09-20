// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "startup_portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QTimer>
#include <QVariantMap>

namespace isotone::ui {

namespace {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kBackgroundInterface = QStringLiteral("org.freedesktop.portal.Background");

// The desktop may ask the person before it answers, so this is generous. It is
// a backstop against a portal that never answers at all, not a normal path.
constexpr int kAnswerTimeoutMs = 30000;

QString token() { return QStringLiteral("isotone%1").arg(QRandomGenerator::global()->generate()); }

// Waits for the Request object's Response signal. The path is known before the
// call is made, so nothing can arrive before this is listening.
class Answer : public QObject {
    Q_OBJECT

public:
    uint response = 1;
    QVariantMap results;
    bool answered = false;
    QEventLoop loop;

public slots:
    void onResponse(uint code, const QVariantMap& values) {
        response = code;
        results = values;
        answered = true;
        loop.quit();
    }
};

// The Response results, for the log. Which keys the portal sends is its own
// business, so nothing is decided on them: whether autostart is on is read back
// off the file the portal wrote (autostart_xdg.h).
QString described(const QVariantMap& results) {
    QStringList parts;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) parts << it.key() + QLatin1Char('=') + it.value().toString();
    parts.sort();
    return parts.join(QLatin1Char(' '));
}

}  // namespace

bool request_autostart(bool on, const QStringList& command, QString* detail) {
    const auto fail = [detail](const QString& why) {
        if (detail != nullptr) *detail = why;
        return false;
    };

    // One at a time: the wait is a nested event loop, so a second click on the
    // toggle would otherwise run a second request inside the first.
    static bool in_flight = false;
    if (in_flight) return fail(QStringLiteral("a request is already open"));
    in_flight = true;
    const struct Guard {
        ~Guard() { in_flight = false; }
    } guard;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return fail(QStringLiteral("no session bus"));

    const QString handle = token();
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString request = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handle);

    Answer answer;
    bus.connect(kPortalService, request, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), &answer,
                SLOT(onResponse(uint, QVariantMap)));

    QVariantMap options{
        {QStringLiteral("handle_token"), handle},
        {QStringLiteral("autostart"), on},
        // Shown by the desktops that ask before granting it.
        {QStringLiteral("reason"), QStringLiteral("Isotone equalizes the machine's audio from sign-in.")},
    };
    // Only meaningful when turning it on, and the portal ignores it otherwise.
    if (on) options.insert(QStringLiteral("commandline"), command);

    QDBusMessage call =
        QDBusMessage::createMethodCall(kPortalService, kPortalPath, kBackgroundInterface, QStringLiteral("RequestBackground"));
    // No parent window: the toggle is in Settings and the portal's own dialog,
    // where there is one, stands on its own.
    call << QString() << options;

    QString error;
    QDBusPendingCallWatcher watcher(bus.asyncCall(call, kAnswerTimeoutMs));
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, [&](QDBusPendingCallWatcher* w) {
        const QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (!reply.isError()) return;
        error = reply.error().message();
        answer.loop.quit();
    });

    // The backstop: a portal that takes the call and never answers.
    QTimer::singleShot(kAnswerTimeoutMs, &answer.loop, &QEventLoop::quit);
    if (!answer.answered && error.isEmpty()) answer.loop.exec();

    bus.disconnect(kPortalService, request, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), &answer,
                   SLOT(onResponse(uint, QVariantMap)));

    if (!error.isEmpty()) return fail(error);
    if (!answer.answered) return fail(QStringLiteral("the portal did not answer"));
    if (answer.response != 0) return fail(QStringLiteral("the desktop refused the request"));
    if (detail != nullptr) *detail = described(answer.results);
    return true;
}

}  // namespace isotone::ui

#include "startup_portal.moc"
