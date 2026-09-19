// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Runs the Qt Quick Test cases in tests/qml against the Isotone QML module, with
// the app's fonts loaded.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtQuickTest>

#include "eqsession.h"
#include "qmlsingleton.h"
#include <windows.h>

// Offscreen unless the caller picks a platform: on the desktop a window is laid
// out only while Windows lets it draw, and with the display off (seen
// 2026-09-14) positioners never ran and the band strip measured 0 px wide.
// Settings and presets go to a scratch directory, never the owner's %APPDATA%\Isotone.
static const bool kPlatformChosen = [] {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("ISOTONE_DATA_DIR")) {
        const QString dir = QDir::temp().filePath(QStringLiteral("isotone-qml-tests-%1").arg(QCoreApplication::applicationPid()));
        QDir(dir).removeRecursively();
        qputenv("ISOTONE_DATA_DIR", dir.toUtf8());
    }
    // tst_export reads back the file it wrote.
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");
    // Devices work package: devicetool and the outputs come from a script, and
    // Equalizer APO's config directory is a sandbox with a Peace include, never
    // the installed one.
    qputenv("ISOTONE_FAKE_DEVICETOOL", ISOTONE_FAKE_DEVICETOOL_SCRIPT);
    if (!qEnvironmentVariableIsSet("ISOTONE_COMPAT_DIR")) {
        const QString dir = QDir::temp().filePath(QStringLiteral("isotone-qml-compat-%1").arg(QCoreApplication::applicationPid()));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        QFile config(QDir(dir).filePath(QStringLiteral("config.txt")));
        if (config.open(QIODevice::WriteOnly)) config.write("Preamp: -3 dB\r\nInclude: peace.txt\r\nGraphicEQ: 25 0; 40 -1.5; 100 0\r\n");
        qputenv("ISOTONE_COMPAT_DIR", dir.toUtf8());
    }
    // Settings: launch at sign-in writes a test key of this run's, never the real Run key.
    if (!qEnvironmentVariableIsSet("ISOTONE_RUN_KEY"))
        qputenv("ISOTONE_RUN_KEY", QStringLiteral("Software\\Isotone-tests\\Run-%1").arg(QCoreApplication::applicationPid()).toUtf8());
    return true;
}();

// Surround tests: the session edits a layout with no output behind it, so
// nothing is written anywhere and no audio plays.
class TestHooks : public QObject {
    Q_OBJECT
public:
    explicit TestHooks(QQmlEngine* engine) : QObject(engine), engine_(engine) {}
    Q_INVOKABLE void useLayout(int channels, int speakerMask) {
        auto* session = isotoneSingleton<EqSession>(engine_, "EqSession");
        session->useTarget(isotone::ui::OutputTarget{
            "", isotone::ui::Backend::none,
            isotone::ui::OutputLayout{static_cast<uint32_t>(channels), static_cast<uint32_t>(speakerMask), 48000.0}});
    }
    // Band strip: a wheel with pixelDelta (a high-resolution wheel), which
    // QtTest's mouseWheel cannot send.
    Q_INVOKABLE void pixelWheel(QQuickItem* item, qreal x, qreal y, int pixelX, int pixelY) {
        if (!item || !item->window()) return;
        const QPointF scene = item->mapToScene(QPointF(x, y));
        QWheelEvent event(scene, item->window()->mapToGlobal(scene), QPoint(pixelX, pixelY), QPoint(0, 0), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(item->window(), &event);
    }
    // Devices: replaces the sandbox config.txt (ISOTONE_COMPAT_DIR, never the installed one).
    Q_INVOKABLE bool setCompatConfig(const QString& text) {
        const QString dir = qEnvironmentVariable("ISOTONE_COMPAT_DIR");
        if (!QDir::fromNativeSeparators(dir).startsWith(QDir::tempPath(), Qt::CaseInsensitive)) return false;
        QFile config(QDir(dir).filePath(QStringLiteral("config.txt")));
        return config.open(QIODevice::WriteOnly | QIODevice::Truncate) && config.write(text.toUtf8()) == text.toUtf8().size();
    }

private:
    QQmlEngine* engine_;
};

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("TestHooks"), new TestHooks(engine));
    }
    void applicationAvailable() {
        for (const char* face : {"Regular", "Medium", "SemiBold"})
            QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Isotone/fonts/InstrumentSans-%1.ttf").arg(QLatin1String(face)));
        QFont font(QStringLiteral("Instrument Sans"));
        font.setPixelSize(13);
        QGuiApplication::setFont(font);
    }
    // Settings: the run's test key goes when the tests end.
    void cleanupTestCase() {
        const std::wstring key = qEnvironmentVariable("ISOTONE_RUN_KEY").toStdWString();
        if (key.rfind(L"Software\\Isotone-tests\\", 0) != 0) return;
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Isotone-tests");   // only when no other run's key is in it
    }
};

QUICK_TEST_MAIN_WITH_SETUP(isotone_ui, Setup)

#include "qml_main.moc"
