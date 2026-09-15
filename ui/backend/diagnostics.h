// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Settings, About: the engines' versions and outputs, protected audio, and the
// text Copy diagnostics puts on the clipboard. Read-only.
//
// Where each comes from:
//   IsoAPO          registered when HKCR\CLSID\{IsoAPO post-mix}\InprocServer32
//                   names a DLL (devicetool's read_registration); its version is
//                   that DLL's file version resource, when it has one
//   Equalizer APO   installed when HKLM\SOFTWARE\EqualizerAPO exists; its version
//                   is EqualizerAPO.dll's in InstallPath, else the uninstall
//                   entry's DisplayVersion
//   outputs         active render endpoints: IsoAPO installed (as the sidebar
//                   lists them), and Equalizer APO in a slot without IsoAPO
//   protected audio HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio
//                   DisableProtectedAudioDG = 1 is disabled, as upstream's
//                   DeviceAPOInfo::checkProtectedAudioDG and devicetool status's
//                   protected_audiodg_disabled read it

#pragma once

#include <string>
#include <vector>

#include "devices.h"

namespace isotone::ui {

enum class ProtectedAudio { enabled, disabled };

struct EngineSummary {
    bool isoapo_registered = false;
    std::string isoapo_dll;        // UTF-8
    std::string isoapo_version;    // empty: the DLL has no version resource
    int isoapo_outputs = 0;
    bool equalizerapo_installed = false;
    std::string equalizerapo_version;
    int equalizerapo_outputs = 0;
    ProtectedAudio protected_audio = ProtectedAudio::enabled;
};

EngineSummary read_engine_summary(const std::vector<isotone::devices::Endpoint>& endpoints);

struct DiagnosticsInput {
    std::string app_version;
    std::string qt_version;
    std::string windows;
    EngineSummary engines;
    std::vector<isotone::devices::Endpoint> endpoints;
};

std::string diagnostics_text(const DiagnosticsInput& in);

// The About rows: "0.1.0 · 2 outputs", "2 outputs" without a version, "Not installed".
std::string isoapo_row(const EngineSummary& s);
std::string equalizerapo_row(const EngineSummary& s);

// "1.4.2" for 1.4.2.0; the fourth part only when it is not 0.
std::string version_text(unsigned major, unsigned minor, unsigned patch, unsigned build);

}  // namespace isotone::ui
