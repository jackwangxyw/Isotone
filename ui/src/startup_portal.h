// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Launch at sign-in inside a Flatpak: the Background portal
// (https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Background.html).
//
// A sandbox cannot write the desktop's autostart directory, and the one it can
// write, ~/.var/app/<id>/config/autostart, is read by nothing. RequestBackground
// with autostart is the one way in: xdg-desktop-portal writes the entry on the
// host itself, names it after the application ID and turns `commandline` into a
// `flatpak run --command=<argv0> <id> <rest>` line. Reading the state back is
// autostart_xdg.h's job, off the file the portal wrote.
//
// The call is made synchronously, because the toggle that starts it is, and the
// portal answers through a Request object's Response signal: the wait is a
// nested event loop, which keeps the window alive if the desktop decides to ask
// the person first.

#pragma once

#include <QString>
#include <QStringList>

namespace isotone::ui {

// RequestBackground with `autostart` set to `on`. `command` is the app's own
// argv, "isotone" and optionally "--tray"; the portal supplies the `flatpak run`
// around it. True when the portal accepted the request, which is all a caller
// can learn from the call: whether autostart is now on is read back off the
// entry (autostart_xdg.h), which is the state either way. `detail` takes the
// reason on a failure and the Response results on success, for the log.
// Re-entrant calls are refused rather than nested.
bool request_autostart(bool on, const QStringList& command, QString* detail = nullptr);

}  // namespace isotone::ui
