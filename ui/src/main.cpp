// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone: the UI. For checks:
//   --screenshot <file.png>   render the window once it has drawn, save it, exit
//   --screenshot-after <s>    take that screenshot later, to catch what changes
//   --output <endpoint>       edit this output instead of the default one
//   --add-band <hz>,<db>      add a band as the Add band button does (repeatable)
//   --quit-after <seconds>    exit on its own
//   --click <x>,<y>[,right]   click there once the window has drawn (repeatable, in order)
//   --data-dir <dir>          settings and presets there instead of %APPDATA%\Isotone
//   --compat-dir <dir>        Equalizer APO outputs write Isotone.txt there, not in its install
//   --fake-devicetool <file>  devicetool's answers and the Devices outputs from a script
//                             (devicetoolrunner.h, devicesmodel.h); nothing is installed or restarted;
//                             needs --compat-dir (also as ISOTONE_FAKE_DEVICETOOL and ISOTONE_COMPAT_DIR)
//   --first-run               show first run
//   --view <view>[/<tab>]     open a view, and a Settings tab (settings/outputs)
//   --tray                    start hidden in the tray (launch at sign-in with Start in the tray)
//   --key <keys>              press keys ("Ctrl+M") after the clicks (repeatable, in order)
//
// One instance per user and data directory: a second launch shows the running
// window and exits (singleinstance.h). A screenshot run is always its own, and
// registers no global hotkeys and no tray icon.

#include <QCommandLineParser>
#include <QSystemTrayIcon>
#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <QProcess>

#include <windows.h>
#include <shellapi.h>

#include "apppaths.h"
#include "devicesmodel.h"
#include "devicetoolcontroller.h"
#include "equalizerapoconfig.h"
#include "eqsession.h"
#include "outputs.h"
// Settings
#include "globalhotkeys.h"
#include "logomark.h"
#include "presets.h"
#include "shortcutregistry.h"
#include "singleinstance.h"
#include "traymenu.h"

#include <cstdio>
#include <cstring>
#include <memory>

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
    const QCommandLineOption screenshot_after(QStringLiteral("screenshot-after"),
                                              QStringLiteral("Take the screenshot after <seconds>."),
                                              QStringLiteral("seconds"));
    parser.addOption(screenshot_after);
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
    const QCommandLineOption fake_devicetool(QStringLiteral("fake-devicetool"), QStringLiteral("Answer devicetool from <script>."),
                                             QStringLiteral("script"));
    const QCommandLineOption first_run(QStringLiteral("first-run"), QStringLiteral("Show first run."));
    const QCommandLineOption view(QStringLiteral("view"), QStringLiteral("Open <view> (eq, devices, settings, settings/outputs)."),
                                  QStringLiteral("view"));
    parser.addOption(fake_devicetool);
    parser.addOption(first_run);
    parser.addOption(view);
    const QCommandLineOption tray(QStringLiteral("tray"), QStringLiteral("Start in the tray."));
    parser.addOption(tray);
    const QCommandLineOption key(QStringLiteral("key"), QStringLiteral("Press <keys> after the clicks."), QStringLiteral("keys"));
    parser.addOption(key);
    parser.process(app);
    // Before any singleton exists: they read these when created.
    if (parser.isSet(data_dir)) AppPaths::setDataDir(parser.value(data_dir));
    if (parser.isSet(compat_dir)) AppPaths::setCompatConfigDir(parser.value(compat_dir));
    if (parser.isSet(fake_devicetool)) qputenv("ISOTONE_FAKE_DEVICETOOL", parser.value(fake_devicetool).toLocal8Bit());
    // The flag or the environment: the controller and Devices read the environment.
    const bool faked = fakeDevicetool();
    if (const QString refusal = fakeDevicetoolRefusal(); !refusal.isEmpty()) {
        std::fprintf(stderr, "%s\n", qPrintable(refusal));
        return 1;
    }

    // Settings: one instance; the tray keeps the app running with the window closed.
    const bool checking = parser.isSet(screenshot);
    const bool start_hidden = parser.isSet(tray) && !checking;
    SingleInstance instance(SingleInstance::nameFor(AppPaths::dataDir()));
    if (!checking) {
        if (instance.notifyRunning(!start_hidden)) return 0;
        if (!instance.listen()) {
            std::fprintf(stderr, "another Isotone holds %s and did not answer\n", qPrintable(SingleInstance::nameFor(AppPaths::dataDir())));
            return 1;
        }
    }
    QApplication::setQuitOnLastWindowClosed(false);

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
    if (start_hidden) engine.setInitialProperties({{QStringLiteral("visible"), false}});
    engine.loadFromModule("Isotone", "Main");
    if (engine.rootObjects().isEmpty()) return 1;

    // Restart Windows and Equalizer APO's uninstaller, only from their buttons in
    // the app itself; with a scripted devicetool they only say so.
    if (auto* devicetool = engine.singletonInstance<DevicetoolController*>("Isotone", "Devicetool")) {
        QObject::connect(devicetool, &DevicetoolController::restartWindowsRequested, &app, [faked] {
            if (faked) {
                std::fprintf(stdout, "restart Windows requested (not with --fake-devicetool)\n");
                return;
            }
            QProcess::startDetached(QStringLiteral("shutdown.exe"), {QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0")});
        });
    }
    if (auto* devices = engine.singletonInstance<DevicesModel*>("Isotone", "Devices")) {
        QObject::connect(devices, &DevicesModel::equalizerApoUninstallerRequested, &app, [faked](const QString& command) {
            QStringList parts = QProcess::splitCommand(command);
            if (faked || parts.isEmpty()) {
                std::fprintf(stdout, "Equalizer APO's uninstaller requested: %s\n", qPrintable(command));
                return;
            }
            const QString program = parts.takeFirst();
            // ShellExecute, so the uninstaller's own request for elevation is honoured.
            ShellExecuteW(nullptr, L"open", program.toStdWString().c_str(), parts.join(QLatin1Char(' ')).toStdWString().c_str(),
                          nullptr, SW_SHOWNORMAL);
        });
    }
    if (parser.isSet(first_run)) QMetaObject::invokeMethod(engine.rootObjects().constFirst(), "showFirstRun");
    if (parser.isSet(view)) {
        const QStringList parts = parser.value(view).split(QLatin1Char('/'));
        if (auto* ui = engine.singletonInstance<QObject*>("Isotone", "UiState")) {
            ui->setProperty("view", parts[0]);
            if (parts.size() > 1) ui->setProperty("settingsTab", parts[1]);
        }
    }
    // Settings: global hotkeys, the tray icon and its menu, a later launch's request.
    auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    const auto show_window = [root_window] {
        if (root_window->windowState() & Qt::WindowMinimized)
            root_window->showNormal();
        else
            root_window->show();
        root_window->raise();
        root_window->requestActivate();
    };
    auto* shortcuts = engine.singletonInstance<ShortcutRegistry*>("Isotone", "ShortcutRegistry");
    std::unique_ptr<GlobalHotkeys> hotkeys;
    if (!checking) hotkeys = std::make_unique<GlobalHotkeys>(shortcuts);
    TrayMenu tray_menu(engine.singletonInstance<EqSession*>("Isotone", "EqSession"),
                       engine.singletonInstance<Outputs*>("Isotone", "Outputs"),
                       engine.singletonInstance<Presets*>("Isotone", "Presets"), shortcuts);
    QSystemTrayIcon tray_icon(logo_mark_icon());
    tray_menu.attach(&tray_icon);
    if (!checking) tray_icon.show();
    QObject::connect(&tray_menu, &TrayMenu::openRequested, &app, show_window);
    // Quit asks about unsaved changes first, as closing does.
    QObject::connect(&tray_menu, &TrayMenu::quitRequested, &app, [root_window] { QMetaObject::invokeMethod(root_window, "requestQuit"); });
    QObject::connect(&instance, &SingleInstance::showRequested, &app, show_window);

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
        QTimer::singleShot(static_cast<int>(800 + 100 * i), &app, [window, spec = clicks[i], i] {
            const QStringList parts = spec.split(QLatin1Char(','));
            if (parts.size() < 2) return;
            const QPointF at(parts[0].toDouble(), parts[1].toDouble());
            const Qt::MouseButton button = parts.size() > 2 && parts[2] == QLatin1String("right") ? Qt::RightButton : Qt::LeftButton;
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), button, button, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
            // Seconds apart on the events' clock, so the next click is not taken for a double click
            // (Devices work package: a dialog's button clicked after the button that opened it).
            press.setTimestamp(static_cast<quint64>(10000 * (i + 1)));
            release.setTimestamp(static_cast<quint64>(10000 * (i + 1) + 10));
            QGuiApplication::sendEvent(window, &press);
            QGuiApplication::sendEvent(window, &release);
        });
    }
    const QStringList keys = parser.values(key);
    for (qsizetype i = 0; i < keys.size(); ++i) {
        QTimer::singleShot(static_cast<int>(800 + 100 * (clicks.size() + i)), &app, [window, spec = keys[i]] {
            const QKeySequence sequence = QKeySequence::fromString(spec, QKeySequence::PortableText);
            if (sequence.isEmpty()) return;
            const QKeyCombination c = sequence[0];
            QKeyEvent press(QEvent::KeyPress, c.key(), c.keyboardModifiers());
            QKeyEvent release(QEvent::KeyRelease, c.key(), c.keyboardModifiers());
            QGuiApplication::sendEvent(window, &press);
            QGuiApplication::sendEvent(window, &release);
        });
    }

    if (parser.isSet(screenshot)) {
        const QString file = parser.value(screenshot);
        // A few frames in, so fonts and layout have settled, or later when the run
        // is watching something that changes (the spectrum after the music stops).
        const int delay_ms =
            parser.isSet(screenshot_after) ? static_cast<int>(parser.value(screenshot_after).toDouble() * 1000) : 1500;
        QTimer::singleShot(delay_ms, &app, [window, file] {
            const QImage image = window->grabWindow();
            const bool saved = image.save(file);
            std::fprintf(saved ? stdout : stderr, "%s %s (%dx%d)\n", saved ? "saved" : "could not save", qPrintable(file),
                         image.width(), image.height());
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
