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
#include <utility>
#include <vector>

namespace isotone::compat {

inline constexpr char kIsotoneFileName[] = "Isotone.txt";
inline constexpr char kConfigBackupName[] = "config.txt.isotone-backup";

// Reads a whole file. Returns a Win32 error code.
DWORD read_file_bytes(const std::filesystem::path& path, std::string* out);

// Replaces `path` with `bytes` so a reader sees the old file or the new one,
// never a partial write: the bytes go to a new `<name>.<pid>.<tid>.tmp`, a name
// no other writer running at the same time uses, then MoveFileExW with
// MOVEFILE_REPLACE_EXISTING swaps it in. Any open handle on the target makes a
// replace fail (Equalizer APO reads config files with FILE_SHARE_READ only), for
// the few milliseconds a read takes, so that is retried for up to `retry_ms`. A
// replace that cannot succeed (a read-only file, a directory, an ACL that denies
// delete) fails at once with ERROR_ACCESS_DENIED. On failure the target is
// unchanged and no .tmp is left behind. Concurrent calls on one path each land
// whole, the last rename winning; CompatWriter locks around its read and write.
//
// The .tmp is made in `temp_dir` (empty: %LOCALAPPDATA%\Isotone\compat-tmp,
// created on first use) when that is on the target's volume, so the only name
// that changes in the target's directory is the target's: every name created
// in Equalizer APO's config directory makes it reload once more (measured). It
// gets the DACL of the file it replaces, or the DACL a file created in the
// target's directory would get, auto-inherited flag included, before it is
// moved in. On another volume, or one with no volume GUID path (a network
// share), the .tmp is made next to the target and inherits from there.
DWORD write_file_atomically(const std::filesystem::path& path, const std::string& bytes,
                            DWORD retry_ms = 200, const std::filesystem::path& temp_dir = {});

// What config.txt says, as far as attaching Isotone is concerned. Lines are
// recognised the way upstream's FilterEngine reads them: the key is the text
// before the first ':', trimmed, compared case-sensitively.
struct ConfigInspection {
    DWORD error = ERROR_SUCCESS;
    bool isotone_included = false;   // an Include line naming Isotone.txt that every device reaches,
                                     // in the post-mix and capture instances and not in pre-mix
    bool isotone_included_conditionally = false;   // one under a Device line other than `all`,
                                                   // inside an If, or under a Stage line: only
                                                   // some devices or instances reach it
    bool peace_included = false;     // an Include line naming peace.txt: Peace and
                                     // Isotone would fight over the same devices
    bool attached_by_isotone = false;// the block attach_include appends is the file's tail
    bool has_stage_lines = false;    // Stage: can hide what follows from the post-mix APO
    bool has_conditionals = false;   // If:/ElseIf:/Else:/EndIf: can hide it too
    unsigned open_ifs = 0;           // If lines no EndIf closes by the end of the file, summed over
                                     // open_ifs_under
    // The Device pattern ("all" for none) the open Ifs were opened under, and
    // how many, in the order first opened. Upstream skips If and EndIf lines on
    // a device the pattern does not match. Where an EndIf may close an If on
    // only some of the devices, the If stays counted.
    std::vector<std::pair<std::string, unsigned>> open_ifs_under;
    bool stage_changed_at_end = false;   // a Stage line leaves the end of the file reaching other
                                         // instances than a file with no Stage line
    // The Device patterns whose Stage lines leave the end of the file reaching
    // other instances for their devices, in order; only "all" when a Stage line
    // every device reached, or one under `Device: all`, does.
    std::vector<std::string> stage_changed_under;
    std::vector<std::string> includes;   // every Include value, in order
};
ConfigInspection inspect_config(const std::filesystem::path& config_dir);
// The same, for config.txt's bytes already in hand. `config_dir` resolves
// Include paths.
ConfigInspection inspect_config_text(const std::string& bytes, const std::filesystem::path& config_dir);

struct AttachResult {
    DWORD error = ERROR_SUCCESS;
    bool appended = false;           // false when Isotone.txt was already included
    std::filesystem::path backup;    // copy of config.txt taken just before appending
    ConfigInspection before;
};

// Appends, once, to config.txt (nothing, with ERROR_ALREADY_EXISTS, when
// Isotone.txt is already included conditionally: appending would include it
// twice for the devices or instances that reach the first include):
//
//   # Added by Isotone. Remove these three lines to detach it.
//   Device: all
//   Include: Isotone.txt
//
// When the file's last line has no line break, the block starts with one and
// the comment says "Remove these three lines and the line break before them",
// so the last line stays intact and detach knows to remove the break.
// `Device: all` is needed because a Device line earlier in config.txt that does
// not match would otherwise make upstream skip the Include line itself. An If
// config.txt leaves open would hide the Include too, so one `EndIf:` per open If
// comes first (IfFilterFactory counts them per file); and a Stage line that
// leaves the end of the file for other instances is undone by
// `Stage: post-mix capture` after them. Upstream skips If, EndIf and Stage
// lines on a device a Device line does not match, so each EndIf and Stage line
// is written under the Device pattern the If or Stage line it undoes was under
// (open_ifs_under, stage_changed_under), with a Device line wherever the
// pattern changes, and `Device: all` follows them:
//
//   # Added by Isotone. Remove these six lines to detach it.
//   Device: Speakers
//   EndIf:
//   Stage: post-mix capture
//   Device: all
//   Include: Isotone.txt
//
// The comment's count of lines includes them. That Stage line matches what a
// file with no Stage line matches except a pre-mix instance with no post-mix
// instance installed, which no Stage line can express without also matching
// pre-mix where post-mix is installed. A Stage line inside an If is undone
// for every device its pattern matches, including those the If skipped it on.
//
// Nothing before the end of the file is rewritten: config.txt is opened
// GENERIC_READ | GENERIC_WRITE with FILE_SHARE_READ, checked against the backup
// through that handle, and written only past its end, which keeps its ACL. Line
// endings follow the file's own (CRLF if it has any, else LF, CRLF for an empty
// file).
AttachResult attach_include(const std::filesystem::path& config_dir);

// Removes the appended block, restoring config.txt byte for byte, but only
// while that block is still the exact tail of the file. If Isotone.txt is
// included some other way, or the block was edited, returns ERROR_INVALID_DATA
// and changes nothing.
DWORD detach_include(const std::filesystem::path& config_dir, bool* removed);

}  // namespace isotone::compat
