// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Finding a stock Equalizer APO install. Read-only.
//
// Upstream's installer writes HKLM\Software\EqualizerAPO "InstallPath" and
// "ConfigPath" (Setup/Setup.nsi), and FilterEngine::initialize reads
// "ConfigPath" from the same key to find config.txt. That value, not
// InstallPath + "\config", is what the engine actually uses.

#pragma once

#include <windows.h>

#include <filesystem>

namespace isotone::compat {

struct EqualizerApoInstall {
    LSTATUS error = ERROR_SUCCESS;       // ERROR_FILE_NOT_FOUND: not installed
    std::filesystem::path install_path;  // may be empty on an odd install
    std::filesystem::path config_path;
};

EqualizerApoInstall locate_equalizer_apo();

// True if `candidate` is `root` or anywhere below it. Compared by file identity
// (volume serial and file ID), not by spelling: a text comparison let the 8.3
// short name C:\PROGRA~1\EqualizerAPO\config through a guard and onto the live
// install. Spelling differences in case, separators, `..`, short names,
// junctions and subst drives all resolve to the same identity. Directories that
// do not exist yet are judged by their nearest existing ancestor.
bool path_is_inside(const std::filesystem::path& candidate, const std::filesystem::path& root);

// True if `dir` is inside the installed Equalizer APO's install or config tree.
// Tools use it to refuse a sandbox path that is really the live install.
bool is_live_install_path(const std::filesystem::path& dir);

}  // namespace isotone::compat
