// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone: the UI. For checks:
//   --screenshot <file.png>   render the window once it has drawn, save it, exit
//   --output <endpoint>       edit this output instead of the default one
//   --add-band <hz>,<db>      add a band as the Add band button does
//   --quit-after <seconds>    exit on its own

#include <QCommandLineParser>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include "eqsession.h"
#include "outputs.h"

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    // A screenshot is compared pixel for pixel with the 1440 x 900 boards, so it
    // is taken at a device pixel ratio of 1 whatever the display scaling.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0) qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Isotone"));

    QCommandLineParser parser;
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Save the window to <file> and exit."),
                                        QStringLiteral("file"));
    parser.addOption(screenshot);
    const QCommandLineOption output(QStringLiteral("output"), QStringLiteral("Edit <endpoint>."), QStringLiteral("endpoint"));
    const QCommandLineOption add_band(QStringLiteral("add-band"), QStringLiteral("Add a band at <hz,db>."), QStringLiteral("hz,db"));
    const QCommandLineOption quit_after(QStringLiteral("quit-after"), QStringLiteral("Exit after <seconds>."), QStringLiteral("seconds"));
    parser.addOption(output);
    parser.addOption(add_band);
    parser.addOption(quit_after);
    parser.process(app);

    for (const char* face : {"Regular", "Medium", "SemiBold"}) {
        const QString path = QStringLiteral(":/qt/qml/Isotone/fonts/InstrumentSans-%1.ttf").arg(QLatin1String(face));
        if (QFontDatabase::addApplicationFont(path) < 0) {
            std::fprintf(stderr, "could not load %s\n", qPrintable(path));
            return 1;
        }
    }
    QFont font(QStringLiteral("Instrument Sans"));
    font.setPixelSize(13);
    font.setFeature(QFont::Tag("tnum"), 1);
    QGuiApplication::setFont(font);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Isotone", "Main");
    if (engine.rootObjects().isEmpty()) return 1;

    if (parser.isSet(output)) {
        auto* outputs = engine.singletonInstance<Outputs*>("Isotone", "Outputs");
        if (!outputs || !outputs->selectGuid(parser.value(output).toStdWString())) {
            std::fprintf(stderr, "%s is not a working output\n", qPrintable(parser.value(output)));
            return 1;
        }
    }
    if (parser.isSet(add_band)) {
        const QStringList parts = parser.value(add_band).split(QLatin1Char(','));
        auto* session = engine.singletonInstance<EqSession*>("Isotone", "EqSession");
        if (parts.size() != 2 || !session) return 1;
        session->addBand(parts[0].toDouble(), parts[1].toDouble());
        std::fprintf(stdout, "added a band at %s Hz, %s dB\n", qPrintable(parts[0]), qPrintable(parts[1]));
    }
    if (parser.isSet(quit_after)) {
        QTimer::singleShot(static_cast<int>(parser.value(quit_after).toDouble() * 1000), &app, [] { QCoreApplication::exit(0); });
    }

    if (parser.isSet(screenshot)) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        const QString file = parser.value(screenshot);
        // A few frames in, so fonts and layout have settled.
        QTimer::singleShot(1500, &app, [window, file] {
            const QImage image = window->grabWindow();
            const bool saved = image.save(file);
            std::fprintf(saved ? stdout : stderr, "%s %s (%dx%d)\n", saved ? "saved" : "could not save", qPrintable(file),
                         image.width(), image.height());
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
