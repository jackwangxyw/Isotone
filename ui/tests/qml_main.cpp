// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Runs the Qt Quick Test cases in tests/qml against the Isotone QML module, with
// the app's fonts loaded.

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QtQuickTest>

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
    return true;
}();

class Setup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        for (const char* face : {"Regular", "Medium", "SemiBold"})
            QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Isotone/fonts/InstrumentSans-%1.ttf").arg(QLatin1String(face)));
        QFont font(QStringLiteral("Instrument Sans"));
        font.setPixelSize(13);
        QGuiApplication::setFont(font);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(isotone_ui, Setup)

#include "qml_main.moc"
