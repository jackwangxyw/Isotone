#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# A stand-in org.freedesktop.portal.GlobalShortcuts on the session bus, speaking
# the protocol as xdg-desktop-portal documents it: CreateSession and
# BindShortcuts answer through a Request object's Response signal, and a bound
# shortcut fires Activated. It binds every shortcut except those whose id is in
# REFUSE, and presses ACTIVATE a moment after binding. Logs what it is asked to
# MOCK_LOG. Run by run_portal_test.sh; needs python3-dbus and python3-gi.
import os
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BUS = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.GlobalShortcuts"
REFUSE = set(filter(None, os.environ.get("REFUSE", "").split(",")))
ACTIVATE = os.environ.get("ACTIVATE", "")
log = open(os.environ["MOCK_LOG"], "a", buffering=1)


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


DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName(BUS, bus)
portal = Portal(bus)
log.write("ready\n")
GLib.MainLoop().run()
