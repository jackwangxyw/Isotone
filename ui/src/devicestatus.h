// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// An output's engine status as the Devices view, Settings Outputs and first run
// show it, from the real engine state: devicetool status's JSON, or
// windows/devices' EngineInfo (the same decisions, read in process).
//
// Status (the prototype's STATUS), first match wins:
//   unplugged         the endpoint is not active (disabled, unplugged)            off
//   interrupted       isoapo.state interrupted (devicetool's journal)             warn
//   unrecorded        isoapo.state unrecorded                                     warn
//   conflict          isoapo.state alongside_equalizerapo                         bad
//   enhancements_off  isoapo.state installed, enhancements disabled             warn
//   installed         isoapo.state installed                                      ok
//   replaced          isoapo.state replaced_by_equalizerapo                       warn
//   detached          isoapo.state detached                                       warn
//   active            not_installed, Equalizer APO in a slot                      ok
//   not_installed     not_installed                                               off
//
// Actions (the prototype's devDetail), and what each runs:
//   installed         test, uninstall
//   active            replace (the Replace dialog), test
//   detached          repair, test, uninstall; with Equalizer APO in a slot
//                     (devicetool's remedies are then install --replace-equalizerapo
//                     and uninstall, as for replaced): takeBack, keepEapo
//   conflict          removeEapo, uninstall
//   replaced          takeBack, keepEapo
//   interrupted       undo
//   unrecorded        copyDiagnostics
//   enhancements_off  enableEnhancements
//   not_installed     install
//   unplugged         none

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

#include "devices.h"

struct DeviceFacts {
    QString guid;                 // {lower-case}
    QString name;                 // "CABLE Input (VB-Audio Virtual Cable)"
    bool present = true;          // active
    bool default_console = false;
    int channels = 0;
    int sample_rate = 0;
    isotone::devices::IsoApoState isoapo = isotone::devices::IsoApoState::not_installed;
    isotone::devices::Backend backend = isotone::devices::Backend::none;
    bool enhancements_disabled = false;
    // From status only: the commands that fix the state, the effect slots
    // IsoAPO and Equalizer APO are in ("MFX", "LFX + GFX"), IsoAPO's install
    // mode ("SFX_MFX") and the mode an install would pick.
    QStringList remedies;
    QString effect_slots;
    QString install_mode;
    QString default_mode;
};

// devicetool status's JSON. False when it is not a status object.
bool factsFromStatusJson(const QByteArray& json, DeviceFacts* out);
// windows/devices' endpoint read (name, state, format, engine).
DeviceFacts factsFromEndpoint(const isotone::devices::Endpoint& e);

QString statusKey(const DeviceFacts& f);
QString statusLabel(const QString& key);   // "Replaced by Equalizer APO"
QString statusDot(const QString& key);     // StatusDot's ok, warn, bad, off

bool equalizerApoPresent(const DeviceFacts& f);   // in one of the output's effect slots
QString engineColumn(const DeviceFacts& f);       // "Native", "Equalizer APO", "Native + Equalizer APO", or a dash
QString engineDetail(const DeviceFacts& f);       // "Native (IsoAPO)", "IsoAPO + Equalizer APO", or a dash
QString formatLabel(const DeviceFacts& f);        // "48 kHz · 2 ch"
// Settings Outputs' Now: "IsoAPO", "Equalizer APO", "IsoAPO + Equalizer APO" or "Off".
QString nowEngine(const DeviceFacts& f);
bool working(const DeviceFacts& f);               // installed or active: the sidebar lists it

QStringList actions(const DeviceFacts& f);

// What an action runs: {"kind": install|repair|uninstall|replace|test, "args": [...]}
// for the controller; empty for replace on an active output (a dialog) and
// copyDiagnostics.
QVariantMap operation(const DeviceFacts& f, const QString& action);

// Settings Outputs: what choosing `want` (IsoAPO, Equalizer APO, Off) runs.
//   {"guid", "commands": [[args], ...], "attach": bool, "removeBlock": bool,
//    "result": "installed" | "attached" | "removed", "changed": bool}
QVariantMap planChange(const DeviceFacts& f, const QString& want);
