// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The state a sink starts with when no UI is running: one file per sink holding
// a ParamBlock as its bytes, the counterpart of windows/transport's file under
// ProgramData.
//
// A file that exists is the sink's state, flat included. No file means the sink
// has never been saved and the daemon starts flat. A file that is not a valid
// block for this build (another version, truncated) counts as flat too.
//
// The daemon runs in the user's session, not as a system service, so these live
// under $XDG_CONFIG_HOME rather than in a machine-wide directory. That is the
// whole reason the Windows side needs ProgramData: audiodg runs as LocalService
// and cannot read a user directory.

#pragma once

#include <string>

#include "isotone/param_block.h"

namespace isotone::posix {

// $XDG_CONFIG_HOME/isotone/devices, or $HOME/.config/isotone/devices when
// XDG_CONFIG_HOME is unset or not absolute, as the XDG base directory
// specification requires. Empty when neither is usable.
std::string persisted_state_dir();

// "<dir>/<key>.bin" with sanitize_key's key (shared_region.h), so the file and
// the shared region name the same sink. Empty when `node_name` is empty.
std::string persisted_state_path(const std::string& dir, const std::string& node_name);

enum class PersistedRead {
    Absent,    // no file: never saved
    Loaded,    // `out` holds the saved block
    Invalid,   // a file that is not a valid block for this build, or unreadable
};

// Reads exactly one block's worth of bytes. The seqlock and host fields of `out`
// are zeroed: they describe a live region, not a file.
PersistedRead read_persisted_state(const std::string& path, ParamBlock* out);

// Writes the block's parameters (not its seqlock or host fields) under this
// build's magic, version and size, atomically: a temporary file beside it,
// fsynced, then a replacing rename, then the directory fsynced so the rename
// itself survives a power cut. Creates the directory and its parents. Returns an
// errno value, 0 on success.
int write_persisted_state(const std::string& path, const ParamBlock& block);

}  // namespace isotone::posix
