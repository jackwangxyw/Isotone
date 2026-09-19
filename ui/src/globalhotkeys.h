// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Global shortcuts: RegisterHotKey on a hidden message-only window, for every
// action with Global on and keys (ShortcutRegistry). WM_HOTKEY activates the
// action. Registered again whenever a binding or Global changes; everything is
// unregistered when this is destroyed.
//
// Keys another app already holds fail with ERROR_HOTKEY_ALREADY_REGISTERED:
// the action is marked globalFailed and the Shortcuts page shows it on its row.
// MOD_NOREPEAT: a held key activates once. None are registered while the
// Shortcuts page waits for keys, so the keys reach the page.
//
// Linux (globalhotkeys_posix.cpp), by session:
//   X11      a passive grab of the keys on the root window (xcb_grab_key), with
//            Caps Lock and Num Lock either way; a grab another client holds
//            fails with BadAccess, which marks the action globalFailed. A held
//            key activates once: presses are ignored until its release.
//   Wayland  the GlobalShortcuts portal (KDE Plasma, GNOME 48 and later): one
//            session, every Global action bound with its keys as the preferred
//            trigger. The desktop decides the final keys and may ask the user;
//            an action it does not bind is marked globalFailed.
//   neither  (a Wayland desktop without the portal) every Global action is
//            marked globalFailed.

#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <QHash>
#include <QObject>
#include <QStringList>

#include <memory>

class ShortcutRegistry;
#if !defined(_WIN32)
class QAbstractNativeEventFilter;
class GlobalShortcutsPortal;
#endif

class GlobalHotkeys : public QObject {
    Q_OBJECT

public:
    explicit GlobalHotkeys(ShortcutRegistry* registry, QObject* parent = nullptr);
    ~GlobalHotkeys() override;

    void apply();
    void unregisterAll();
    // The actions registered now, in the registry's order.
    QStringList registered() const;
    // Delivers WM_HOTKEY for `id` as Windows would (Linux: a key press or the
    // portal's Activated); false when it is not registered.
    bool simulate(const QString& id);

#if defined(_WIN32)
    // A PortableText sequence as RegisterHotKey's modifiers (with MOD_NOREPEAT) and virtual key.
    static bool toNative(const QString& sequence, UINT* modifiers, UINT* vk);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    ShortcutRegistry* registry_;
    HWND hwnd_ = nullptr;
    QHash<int, QString> ids_;   // hotkey id -> action
#else
    // A PortableText sequence as an X keysym and X modifier mask (ShiftMask,
    // ControlMask, Mod1Mask for Alt, Mod4Mask for Meta).
    static bool toKeysym(const QString& sequence, unsigned* modifiers, unsigned long* keysym);
    // The portal's preferred_trigger, as the XDG shortcuts specification writes
    // it: "CTRL+ALT+e". Empty when the keys have no keysym.
    static QString toPortalTrigger(const QString& sequence);
    // Whether XKB options (comma-separated, as _XKB_RULES_NAMES holds them) make
    // Alt+Shift switch the keyboard layout. The second of the two pressed is then
    // taken out of the modifiers, and a binding with both never matches.
    static bool altShiftSwitchesLayout(const QString& xkbOptions);
    // "x11", "portal", or empty when this session has neither.
    QString mechanism() const { return mechanism_; }

private:
    friend class HotkeyFilter;
    struct Grab {
        QString id;
        unsigned keycode = 0;
        unsigned modifiers = 0;
        bool down = false;
    };
    void grabX11();
    void bindPortal();
    bool keyEvent(unsigned keycode, unsigned state, bool press);

    ShortcutRegistry* registry_;
    QString mechanism_;
    QList<Grab> grabs_;
    QStringList bound_;   // portal: the actions the desktop bound
    std::unique_ptr<QAbstractNativeEventFilter> filter_;
    GlobalShortcutsPortal* portal_ = nullptr;
#endif
};
