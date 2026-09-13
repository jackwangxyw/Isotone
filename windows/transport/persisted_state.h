// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The state a device starts with when no UI is running (plan 7.6): one file per
// endpoint holding a ParamBlock as its bytes. The UI writes it whenever the
// device's state is saved; IsoAPO reads it before it creates the endpoint's
// shared region, and whenever it locks with no consistent block to read.
//
// A file that exists is the device's state, flat included. No file means the
// device has never been saved, and IsoAPO starts flat. A file that is not a
// valid block for this build (another version, truncated) counts as flat too.
//
// audiodg runs as LocalService and cannot read user directories, so the files
// live under ProgramData.

#pragma once

#include <windows.h>

#include <string>

#include "isotone/param_block.h"

namespace isotone::win {

// %ProgramData%\IsoAPO\devices. With `selftest`, %LOCALAPPDATA%\IsoAPO-selftest\devices,
// which the self-test build of the APO reads: a test must not touch the real
// devices' files. Empty if the known folder cannot be resolved.
std::wstring persisted_state_dir(bool selftest);

// "<dir>\{guid}.bin" with the GUID lower-cased and braced, whatever form it was
// given in, so the APO (upper case) and the UI (lower case) name the same file.
std::wstring persisted_state_path(const std::wstring& dir, const std::wstring& endpoint_guid);

enum class PersistedRead {
    Absent,    // no file: never saved
    Loaded,    // `out` holds the saved block
    Invalid,   // a file that is not a valid block for this build, or cannot be read
};

// Reads at most one block's worth of bytes. The seqlock and host fields of
// `out` are zeroed: they belong to the live region, not the file.
PersistedRead read_persisted_state(const std::wstring& path, ParamBlock* out);

// Writes the block's parameters (not its seqlock or host fields) atomically:
// a temporary file beside it, then a replacing rename. Creates the directory
// and its parent. Returns a Win32 error code.
DWORD write_persisted_state(const std::wstring& path, const ParamBlock& block);

}  // namespace isotone::win
