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

#pragma once

#include <windows.h>

#include <QHash>
#include <QObject>
#include <QStringList>

class ShortcutRegistry;

class GlobalHotkeys : public QObject {
    Q_OBJECT

public:
    explicit GlobalHotkeys(ShortcutRegistry* registry, QObject* parent = nullptr);
    ~GlobalHotkeys() override;

    void apply();
    void unregisterAll();
    // The actions registered now, in the registry's order.
    QStringList registered() const;
    // Delivers WM_HOTKEY for `id` as Windows would; false when it is not registered.
    bool simulate(const QString& id);

    // A PortableText sequence as RegisterHotKey's modifiers (with MOD_NOREPEAT) and virtual key.
    static bool toNative(const QString& sequence, UINT* modifiers, UINT* vk);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    ShortcutRegistry* registry_;
    HWND hwnd_ = nullptr;
    QHash<int, QString> ids_;   // hotkey id -> action
};
