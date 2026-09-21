#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# A stand-in xdg-desktop-portal on the session bus, speaking the protocol as it
# is documented, for the two interfaces this app uses.
#
# GlobalShortcuts: CreateSession and BindShortcuts answer through a Request
# object's Response signal, and a bound shortcut fires Activated. It binds every
# shortcut except those whose id is in REFUSE, and presses ACTIVATE a moment
# after binding. With REFUSE_SESSION set it answers CreateSession with an error
# instead and sends no Response at all, which is what GNOME and Plasma do to an
# app they have no application ID for.
#
# Background: RequestBackground with autostart writes or removes the entry
# xdg-desktop-portal would, in MOCK_AUTOSTART_DIR and named for MOCK_APP_ID,
# with the `flatpak run` line it builds out of `commandline`. The shape is the
# one measured off xdg-desktop-portal 1.20 on the owner's laptop (decisions.md,
# "Launch at sign-in in a Flatpak"). It refuses while MOCK_LOG.refuse exists,
# which is how the refused path is tested.
#
# Logs what it is asked to MOCK_LOG. Run by run_portal_test.sh; needs
# python3-dbus and python3-gi.
import os
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BUS = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.GlobalShortcuts"
BACKGROUND = "org.freedesktop.portal.Background"
REFUSE = set(filter(None, os.environ.get("REFUSE", "").split(",")))
ACTIVATE = os.environ.get("ACTIVATE", "")
REFUSE_SESSION = os.environ.get("REFUSE_SESSION", "") != ""
AUTOSTART_DIR = os.environ.get("MOCK_AUTOSTART_DIR", "")
APP_ID = os.environ.get("MOCK_APP_ID", "io.github.jackwangxyw.Isotone")
LOG_PATH = os.environ["MOCK_LOG"]
log = open(LOG_PATH, "a", buffering=1)


class Request(dbus.service.Object):
    @dbus.service.signal("org.freedesktop.portal.Request", signature="ua{sv}")
    def Response(self, response, results):
        pass


class Session(dbus.service.Object):
    @dbus.service.method("org.freedesktop.portal.Session", in_signature="", out_signature="")
    def Close(self):
        log.write(f"Close {self._object_path}\n")


class Portal(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, PATH)
        self.bus = bus
        self.objects = []

    def _request(self, sender, token):
        s = sender[1:].replace(".", "_")
        path = f"/org/freedesktop/portal/desktop/request/{s}/{token}"
        r = Request(self.bus, path)
        self.objects.append(r)
        return r, path

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return dbus.UInt32(1)

    @dbus.service.method(IFACE, in_signature="a{sv}", out_signature="o", sender_keyword="sender")
    def CreateSession(self, options, sender=None):
        if REFUSE_SESSION:
            # What GNOME and Plasma both do to an app they have no application
            # ID for: the method itself errors and no Response is ever sent.
            log.write("CreateSession refused\n")
            raise dbus.DBusException("Not allowed", name="org.freedesktop.portal.Error.NotAllowed")
        r, path = self._request(sender, str(options["handle_token"]))
        s = sender[1:].replace(".", "_")
        session = f"/org/freedesktop/portal/desktop/session/{s}/{options['session_handle_token']}"
        self.objects.append(Session(self.bus, session))
        log.write(f"CreateSession -> {session}\n")
        GLib.timeout_add(50, lambda: (r.Response(dbus.UInt32(0), {"session_handle": session}), False)[1])
        return dbus.ObjectPath(path)

    @dbus.service.method(IFACE, in_signature="oa(sa{sv})sa{sv}", out_signature="o", sender_keyword="sender")
    def BindShortcuts(self, session, shortcuts, parent, options, sender=None):
        r, path = self._request(sender, str(options["handle_token"]))
        bound = []
        for sid, props in shortcuts:
            log.write(f"BindShortcuts {sid} trigger={props.get('preferred_trigger')} description={props.get('description')}\n")
            if str(sid) not in REFUSE:
                bound.append(dbus.Struct((sid, dbus.Dictionary({"trigger_description": props.get("preferred_trigger", "")},
                                                                   signature="sv")), signature="sa{sv}"))
        results = {"shortcuts": dbus.Array(bound, signature="(sa{sv})")}
        GLib.timeout_add(50, lambda: (r.Response(dbus.UInt32(0), results), False)[1])
        if ACTIVATE:
            GLib.timeout_add(1500, lambda: (self.Activated(session, ACTIVATE, dbus.UInt64(0), {}),
                                            log.write(f"Activated {ACTIVATE}\n"), False)[2])
        return dbus.ObjectPath(path)

    @dbus.service.signal(IFACE, signature="osta{sv}")
    def Activated(self, session, shortcut_id, timestamp, options):
        pass

    @dbus.service.method(BACKGROUND, in_signature="sa{sv}", out_signature="o", sender_keyword="sender")
    def RequestBackground(self, parent, options, sender=None):
        r, path = self._request(sender, str(options["handle_token"]))
        autostart = bool(options.get("autostart", False))
        command = [str(a) for a in options.get("commandline", [])]
        reason = options.get("reason", "")
        log.write(f"RequestBackground autostart={autostart} commandline={' '.join(command)} reason={reason}\n")
        refused = os.path.exists(LOG_PATH + ".refuse")
        if not refused:
            write_autostart(autostart, command)
        results = {} if refused else {"background": dbus.Boolean(True), "autostart": dbus.Boolean(autostart)}
        code = dbus.UInt32(1 if refused else 0)
        GLib.timeout_add(50, lambda: (r.Response(code, results), False)[1])
        return dbus.ObjectPath(path)


def write_autostart(autostart, command):
    """The entry xdg-desktop-portal writes on the host, or its removal."""
    if not AUTOSTART_DIR:
        return
    entry = os.path.join(AUTOSTART_DIR, APP_ID + ".desktop")
    if not autostart:
        if os.path.exists(entry):
            os.remove(entry)
        return
    os.makedirs(AUTOSTART_DIR, exist_ok=True)
    # The command goes through `flatpak run`, and the two X- keys mark the entry
    # as the portal's own.
    exec_line = " ".join(["flatpak", "run", f"--command={command[0]}", APP_ID] + command[1:])
    lines = [
        "[Desktop Entry]",
        "Type=Application",
        f"Name={APP_ID}",
        f"X-XDP-Autostart={APP_ID}",
        f"Exec={exec_line}",
        f"X-Flatpak={APP_ID}",
    ]
    with open(entry, "w") as f:
        f.write("\n".join(lines) + "\n")


DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName(BUS, bus)
portal = Portal(bus)
log.write("ready\n")
GLib.MainLoop().run()
