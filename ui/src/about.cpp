// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "about.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QSettings>
#include <QSysInfo>

#if defined(_WIN32)
#include <objbase.h>

#include "diagnostics.h"

namespace {

// Endpoints need COM on this thread; the offscreen platform the tests use does not start it.
std::vector<isotone::devices::Endpoint> endpoints() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<isotone::devices::Endpoint> out;
    isotone::devices::enumerate_render_endpoints(&out);
    if (SUCCEEDED(com)) CoUninitialize();
    return out;
}

}  // namespace

About::About(QObject* parent) : QObject(parent) {}

QString About::version() { return QStringLiteral(ISOTONE_VERSION); }

void About::refresh() {
    const isotone::ui::EngineSummary s = isotone::ui::read_engine_summary(endpoints());
    isoapo_ = QString::fromStdString(isotone::ui::isoapo_row(s));
    equalizer_apo_ = QString::fromStdString(isotone::ui::equalizerapo_row(s));
    protected_audio_disabled_ = s.protected_audio == isotone::ui::ProtectedAudio::disabled;
    emit changed();
}

QString About::diagnostics() const {
    isotone::ui::DiagnosticsInput in;
    in.app_version = version().toStdString();
    in.qt_version = qVersion();
    // "Windows 11 Version 24H2", and the build with its update revision.
    const QSettings nt(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"), QSettings::NativeFormat);
    QString windows = QSysInfo::prettyProductName() + QStringLiteral(", build ") + QSysInfo::kernelVersion();
    if (nt.contains(QStringLiteral("UBR"))) windows += QLatin1Char('.') + QString::number(nt.value(QStringLiteral("UBR")).toUInt());
    in.windows = windows.toStdString();
    in.endpoints = endpoints();
    in.engines = isotone::ui::read_engine_summary(in.endpoints);
    return QString::fromStdString(isotone::ui::diagnostics_text(in));
}

#else
#include <pipewire/pipewire.h>

#include "daemon_region.h"
#include "pipewire_outputs.h"

namespace {

struct DaemonState {
    bool running = false;          // Isotone's own sink is there
    std::string fed;               // node.name of the sink the daemon feeds
    std::string fed_description;
    isotone::ui::DaemonRegion region;
    std::vector<isotone::ui::PipewireSink> sinks;
    std::string default_sink;
};

DaemonState read_daemon_state() {
    DaemonState out;
    isotone::ui::PipewireOutputs pipewire;
    if (!pipewire.start()) return out;
    pipewire.wait_ready();
    out.sinks = pipewire.sinks();
    out.default_sink = pipewire.default_sink();
    pipewire.stop();
    for (const isotone::ui::PipewireSink& sink : out.sinks) {
        if (sink.is_isotone) {
            out.running = true;
            continue;
        }
        if (out.fed.empty() && isotone::ui::read_daemon_region(sink.name, &out.region) == 0) {
            out.fed = sink.name;
            out.fed_description = sink.description;
        }
    }
    return out;
}

}  // namespace

About::About(QObject* parent) : QObject(parent) {}

QString About::version() { return QStringLiteral(ISOTONE_VERSION); }

void About::refresh() {
    const DaemonState d = read_daemon_state();
    daemon_ = !d.running       ? QStringLiteral("Not running")
              : d.fed.empty()  ? QStringLiteral("Running · no output")
                               : QStringLiteral("Running · %1").arg(QString::fromStdString(d.fed_description));
    pipewire_ = QString::fromUtf8(pw_get_library_version());
    emit changed();
}

QString About::diagnostics() const {
    const DaemonState d = read_daemon_state();
    QString text;
    text += QStringLiteral("Isotone %1\n").arg(version());
    text += QStringLiteral("Qt %1\n").arg(QString::fromLatin1(qVersion()));
    text += QStringLiteral("%1, kernel %2\n").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    text += QStringLiteral("Desktop: %1, session: %2\n")
                .arg(qEnvironmentVariable("XDG_CURRENT_DESKTOP"), qEnvironmentVariable("XDG_SESSION_TYPE"));
    text += QStringLiteral("PipeWire library %1\n").arg(QString::fromUtf8(pw_get_library_version()));
    text += QStringLiteral("Daemon: %1\n").arg(d.running ? QStringLiteral("running") : QStringLiteral("not running"));
    if (!d.fed.empty()) {
        text += QStringLiteral("Feeds: %1, %2 Hz, %3 ch, mask 0x%4, heartbeat %5\n")
                    .arg(QString::fromStdString(d.fed))
                    .arg(d.region.sample_rate)
                    .arg(d.region.channels)
                    .arg(d.region.speaker_mask, 0, 16)
                    .arg(d.region.heartbeat);
    }
    text += QStringLiteral("Default sink: %1\n").arg(QString::fromStdString(d.default_sink));
    text += QStringLiteral("Sinks:\n");
    for (const isotone::ui::PipewireSink& sink : d.sinks)
        text += QStringLiteral("  %1 (%2)\n").arg(QString::fromStdString(sink.name), QString::fromStdString(sink.description));
    return text;
}
#endif

void About::copyDiagnostics() const { QGuiApplication::clipboard()->setText(diagnostics()); }
