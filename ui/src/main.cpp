// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone: the UI. For checks:
//   --screenshot <file.png>   render the window once it has drawn, save it, exit
//   --output <endpoint>       edit this output instead of the default one
//   --add-band <hz>,<db>      add a band as the Add band button does (repeatable)
//   --quit-after <seconds>    exit on its own
//   --click <x>,<y>[,right]   click there once the window has drawn (repeatable, in order)
//   --data-dir <dir>          settings and presets there instead of %APPDATA%\Isotone
//   --compat-dir <dir>        Equalizer APO outputs write Isotone.txt there, not in its install

#include <QCommandLineParser>
#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include "apppaths.h"
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
    // QApplication, not QGuiApplication: the tray icon is a Qt Widgets class.
    QApplication app(argc, argv);
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
    const QCommandLineOption click(QStringLiteral("click"), QStringLiteral("Click at <x,y[,right]>."), QStringLiteral("x,y"));
    parser.addOption(click);
    const QCommandLineOption data_dir(QStringLiteral("data-dir"), QStringLiteral("Keep settings and presets in <dir>."), QStringLiteral("dir"));
    const QCommandLineOption compat_dir(QStringLiteral("compat-dir"), QStringLiteral("Write Equalizer APO outputs to <dir>."), QStringLiteral("dir"));
    parser.addOption(data_dir);
    parser.addOption(compat_dir);
    parser.process(app);
    // Before any singleton exists: they read these when created.
    if (parser.isSet(data_dir)) AppPaths::setDataDir(parser.value(data_dir));
    if (parser.isSet(compat_dir)) AppPaths::setCompatConfigDir(parser.value(compat_dir));

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
    for (const QString& spec : parser.values(add_band)) {
        const QStringList parts = spec.split(QLatin1Char(','));
        auto* session = engine.singletonInstance<EqSession*>("Isotone", "EqSession");
        if (parts.size() != 2 || !session) return 1;
        session->addBand(parts[0].toDouble(), parts[1].toDouble());
        std::fprintf(stdout, "added a band at %s Hz, %s dB\n", qPrintable(parts[0]), qPrintable(parts[1]));
    }
    if (parser.isSet(quit_after)) {
        QTimer::singleShot(static_cast<int>(parser.value(quit_after).toDouble() * 1000), &app, [] { QCoreApplication::exit(0); });
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    const QStringList clicks = parser.values(click);
    for (qsizetype i = 0; i < clicks.size(); ++i) {
        QTimer::singleShot(static_cast<int>(800 + 100 * i), &app, [window, spec = clicks[i]] {
            const QStringList parts = spec.split(QLatin1Char(','));
            if (parts.size() < 2) return;
            const QPointF at(parts[0].toDouble(), parts[1].toDouble());
            const Qt::MouseButton button = parts.size() > 2 && parts[2] == QLatin1String("right") ? Qt::RightButton : Qt::LeftButton;
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), button, button, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
            QGuiApplication::sendEvent(window, &press);
            QGuiApplication::sendEvent(window, &release);
        });
    }

    if (parser.isSet(screenshot)) {
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
