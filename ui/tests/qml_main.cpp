// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Runs the Qt Quick Test cases in tests/qml against the Isotone QML module, with
// the app's fonts loaded.

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QtQuickTest>

// Offscreen unless the caller picks a platform: on the desktop a window is laid
// out only while Windows lets it draw, and with the display off (seen
// 2026-09-14) positioners never ran and the band strip measured 0 px wide.
static const bool kPlatformChosen = [] {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
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
