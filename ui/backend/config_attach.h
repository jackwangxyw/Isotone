// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Attaching Isotone to an Equalizer APO config directory, as the Attach dialog
// shows it: config.txt's lines, the Peace include, the lines attach_include
// appends, and the attach itself with the Peace include kept or removed.
//
// Everything goes through windows/compat (config_files.h); the directory is a
// parameter. Equalizer APO's installer gives Users full control of its config
// directory (read with icacls on 2026-09-15: BUILTIN\Users:(OI)(CI)(F)), so an
// unelevated app can attach; nothing here asks for elevation.

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace isotone::ui {

struct AttachPreview {
    DWORD error = ERROR_SUCCESS;       // reading config.txt; nothing below is meaningful on an error
    std::vector<std::string> lines;    // config.txt as upstream splits it, without a last empty line
    std::vector<bool> peace;           // per line: an Include of peace.txt
    std::vector<std::string> added;    // the lines attach_include would append now
    bool attached = false;             // Isotone.txt is already included for every device
};

// Reads config.txt and works out what attach_include appends by running it on a
// copy in a temporary directory, so the preview is the library's own block.
AttachPreview preview_attach(const std::filesystem::path& config_dir);

// config.txt's bytes without the Include lines that name peace.txt (key and
// value read as config_files.cpp reads them). Other bytes, line endings
// included, are kept.
std::string without_peace_includes(const std::string& bytes);

struct AttachOutcome {
    DWORD error = ERROR_SUCCESS;
    bool peace_removed = false;   // an Include of peace.txt was removed
    bool appended = false;        // attach_include appended its block
};

// With `remove_peace`, first rewrites config.txt without its Peace includes
// (write_file_atomically), then attach_include. Running it again changes
// nothing.
AttachOutcome attach_config(const std::filesystem::path& config_dir, bool remove_peace);

}  // namespace isotone::ui
