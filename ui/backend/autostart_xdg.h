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
//
// In a Flatpak the file is still the state, but it is not this code's to write.
// $XDG_CONFIG_HOME there is ~/.var/app/<id>/config, which no desktop reads, and
// the host's directory is mounted read-only. The entry belongs to the Background
// portal (ui/src/startup_portal.h), which names it after the application ID and
// fills in the `flatpak run` command. Reading it back is the same code either
// way: what changes here is the directory and the file's name.

#pragma once

#include <string>
#include <vector>

namespace isotone::ui {

// The file's name, which is also its desktop entry ID, and the ID has to be the
// application's: the GlobalShortcuts portal takes the ID from the systemd unit
// the desktop started the app in, looks for a desktop file of that name, and
// refuses the app its keys when it finds none. An entry called isotone.desktop
// gives the ID "isotone", and the installed desktop file is
// io.github.jackwangxyw.Isotone.desktop, so nothing resolved and global hotkeys
// were refused at every sign-in (decisions.md, "An application ID is what the
// portal wants").
inline constexpr char kAutostartFileName[] = "io.github.jackwangxyw.Isotone.desktop";

// What the entry has been called before, newest first. Only ever read and
// removed, never written: an entry left under an old name would go on starting
// the app, and with an ID that now resolves to nothing.
//   io.github.isotone.Isotone  the application ID until 2026-09-21, which named
//                              a GitHub account that is not the repository's
//   isotone                    before 2026-09-20, no application ID at all
inline constexpr const char* kLegacyAutostartFileNames[] = {
    "io.github.isotone.Isotone.desktop",
    "isotone.desktop",
};

// $XDG_CONFIG_HOME/autostart, or $HOME/.config/autostart when XDG_CONFIG_HOME is
// unset or not absolute, as the base directory specification requires. Empty
// when neither is usable.
//
// In a Flatpak it is $HOME/.config/autostart whatever XDG_CONFIG_HOME says: the
// desktop reads the host's directory, and the sandbox's own is not it.
std::string autostart_dir();

// "<dir>/io.github.jackwangxyw.Isotone.desktop", or "<dir>/<FLATPAK_ID>.desktop"
// in a Flatpak, which is what the Background portal names the entry it writes
// and is the same name anywhere the ID is the usual one. Empty when `dir` is
// empty.
std::string autostart_path(const std::string& dir);

// The entries older versions wrote, newest first, so they can be read and
// cleaned up. Empty when `dir` is empty.
std::vector<std::string> legacy_autostart_paths(const std::string& dir);

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
