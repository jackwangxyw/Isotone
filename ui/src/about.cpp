// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "about.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QSettings>
#include <QSysInfo>

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

void About::copyDiagnostics() const { QGuiApplication::clipboard()->setText(diagnostics()); }
