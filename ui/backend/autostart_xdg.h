// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Launch at sign-in on Linux: a .desktop file under $XDG_CONFIG_HOME/autostart,
// which is what every desktop this targets reads, GNOME, KDE and Cinnamon alike.
// The counterpart of startup_registration.h's value under HKCU's Run key.
//
// The file is the whole of the state: present means on, absent means off. There
// is nothing to enable separately, and no equivalent of Windows' StartupApproved,
// so a desktop that lets a person turn entries off does it by writing
// Hidden=true, which is read back here as off.

#pragma once

#include <string>

namespace isotone::ui {

// The file's name, which is also its desktop entry id.
inline constexpr char kAutostartFileName[] = "isotone.desktop";

// $XDG_CONFIG_HOME/autostart, or $HOME/.config/autostart when XDG_CONFIG_HOME is
// unset or not absolute, as the base directory specification requires. Empty
// when neither is usable.
std::string autostart_dir();

// "<dir>/isotone.desktop". Empty when `dir` is empty.
std::string autostart_path(const std::string& dir);

// The file's contents: `exec` is the command, with --tray appended when `tray`.
// A value in a desktop entry cannot carry a newline, so one in `exec` would
// otherwise write a second key; it is refused rather than written.
std::string autostart_contents(const std::string& exec, bool tray);

// Writes the file, creating the directory. Returns an errno value, 0 on success;
// EINVAL when `exec` holds a newline.
int write_autostart(const std::string& path, const std::string& exec, bool tray);

// Removes it. A file that is not there is success, as turning off something
// already off is not a failure.
int remove_autostart(const std::string& path);

// True when the file is there and is not switched off with Hidden=true.
bool autostart_enabled(const std::string& path);

// The Exec= line of the file, empty when there is none to read.
std::string autostart_command(const std::string& path);

}  // namespace isotone::ui
