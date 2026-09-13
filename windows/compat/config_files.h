// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Files of a stock Equalizer APO install that the compatibility backend touches
// (plan 5.5): one appended block in config.txt, and Isotone.txt, which Isotone
// owns outright.
//
// Every function takes the config directory as a parameter. Nothing here knows
// where a real install lives; see eapo_install.h for that, and keep the two
// apart so tests and tools default to a sandbox.

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace isotone::compat {

inline constexpr char kIsotoneFileName[] = "Isotone.txt";
inline constexpr char kConfigBackupName[] = "config.txt.isotone-backup";

// Reads a whole file. Returns a Win32 error code.
DWORD read_file_bytes(const std::filesystem::path& path, std::string* out);

// Replaces `path` with `bytes` so a reader sees the old file or the new one,
// never a partial write: the bytes go to `<path>.tmp`, then MoveFileExW with
// MOVEFILE_REPLACE_EXISTING swaps it in. Equalizer APO opens config files
// without FILE_SHARE_DELETE while it reads them, which makes a replace fail for
// the few milliseconds a read takes, so that case is retried for up to
// `retry_ms`. On failure the target is unchanged and no .tmp is left behind.
DWORD write_file_atomically(const std::filesystem::path& path, const std::string& bytes,
                            DWORD retry_ms = 200);

// What config.txt says, as far as attaching Isotone is concerned. Lines are
// recognised the way upstream's FilterEngine reads them: the key is the text
// before the first ':', trimmed, compared case-sensitively.
struct ConfigInspection {
    DWORD error = ERROR_SUCCESS;
    bool isotone_included = false;   // an Include line naming Isotone.txt
    bool peace_included = false;     // an Include line naming peace.txt: Peace and
                                     // Isotone would fight over the same devices
    bool attached_by_isotone = false;// the block attach_include appends is the file's tail
    bool has_stage_lines = false;    // Stage: can hide what follows from the post-mix APO
    bool has_conditionals = false;   // If:/ElseIf:/Else:/EndIf: can hide it too
    std::vector<std::string> includes;   // every Include value, in order
};
ConfigInspection inspect_config(const std::filesystem::path& config_dir);

struct AttachResult {
    DWORD error = ERROR_SUCCESS;
    bool appended = false;           // false when Isotone.txt was already included
    std::filesystem::path backup;    // copy of config.txt taken just before appending
    ConfigInspection before;
};

// Appends, once, to config.txt:
//
//   # Added by Isotone. Remove these three lines to detach it.
//   Device: all
//   Include: Isotone.txt
//
// preceded by a line break so a file without a trailing newline keeps its last
// line intact. Nothing before the end of the file is rewritten; the file is
// opened for append only, which also keeps its ACL. `Device: all` is needed
// because a Device line earlier in config.txt that does not match would
// otherwise make upstream skip the Include line itself. Line endings follow the
// file's own (CRLF if it has any, else LF, CRLF for an empty file).
AttachResult attach_include(const std::filesystem::path& config_dir);

// Removes the appended block, restoring config.txt byte for byte, but only
// while that block is still the exact tail of the file. If Isotone.txt is
// included some other way, or the block was edited, returns ERROR_INVALID_DATA
// and changes nothing.
DWORD detach_include(const std::filesystem::path& config_dir, bool* removed);

}  // namespace isotone::compat
