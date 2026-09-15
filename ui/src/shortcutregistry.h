// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Keyboard shortcuts (Settings, Shortcuts): each action's keys and whether it is
// global, in settings.ini as shortcuts/<id> (a PortableText key sequence, empty
// for none) and shortcuts/<id>/global.
//
// App actions ("app") can be rebound; EQ, Mute, Next and Previous preset can be
// global. Of the selected band's ("band"), only Delete can be rebound: the
// arrow pairs, [ and ] and Shift are fixed, and any sequence on those keys, with
// or without Shift, is theirs.
//
// Every in-app Shortcut, global hotkey and the tray call activate(id); QML
// performs the action (AppShortcuts.qml).

#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class AppSettings;
class QQmlEngine;
class QJSEngine;

class ShortcutRegistry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Bumped on any change, for bindings on the functions below.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // True while the Shortcuts page waits for keys: in-app shortcuts are off.
    Q_PROPERTY(bool capturing READ capturing WRITE setCapturing NOTIFY capturingChanged)

public:
    explicit ShortcutRegistry(AppSettings* settings, QObject* parent = nullptr);
    // The QML singleton, on the engine's AppSettings.
    static ShortcutRegistry* create(QQmlEngine* qml, QJSEngine* js);

    int revision() const { return revision_; }
    bool capturing() const { return capturing_; }
    void setCapturing(bool on);

    // "app" or "band", in the order the page lists them.
    Q_INVOKABLE QStringList ids(const QString& section) const;
    Q_INVOKABLE QString label(const QString& id) const;
    Q_INVOKABLE bool rebindable(const QString& id) const;
    Q_INVOKABLE bool globalCapable(const QString& id) const;

    // PortableText, for Shortcut.sequence; empty when the action has no keys.
    Q_INVOKABLE QString sequence(const QString& id) const;
    // NativeText, for menus.
    Q_INVOKABLE QString nativeText(const QString& id) const;
    // What the key caps show: ["Ctrl", "→"]; a fixed row's own caps.
    Q_INVOKABLE QStringList keyCaps(const QString& id) const;
    Q_INVOKABLE QStringList keyCapsFor(const QString& sequence) const;
    // A key press as a PortableText sequence; empty for a modifier alone or Escape.
    Q_INVOKABLE QString sequenceFor(int key, int modifiers) const;

    Q_INVOKABLE bool isGlobal(const QString& id) const;
    Q_INVOKABLE void setGlobal(const QString& id, bool on);
    // Set by GlobalHotkeys: the keys are held by another app.
    Q_INVOKABLE bool globalFailed(const QString& id) const;
    Q_INVOKABLE void setGlobalFailed(const QString& id, bool failed);

    // The other action holding `sequence`, or empty.
    Q_INVOKABLE QString conflict(const QString& id, const QString& sequence) const;
    // False when `id` cannot be rebound or another action holds the keys.
    Q_INVOKABLE bool rebind(const QString& id, const QString& sequence);
    // Takes the keys from the rebindable action holding them, which is left with
    // none. False when a fixed action holds them.
    Q_INVOKABLE bool replace(const QString& id, const QString& sequence);

    Q_INVOKABLE void activate(const QString& id);

signals:
    void changed();
    // A binding or Global changed: in-app shortcuts and global hotkeys re-apply.
    void bindingsChanged();
    void activated(const QString& id);
    void capturingChanged();

private:
    struct Action {
        QString id;
        QString label;
        QString section;
        QKeySequence defaults;
        bool global_capable = false;
        bool rebindable = true;
        QStringList fixed_caps;   // a fixed row
    };
    const Action* find(const QString& id) const;
    void store(const QString& id, const QString& sequence);
    void bump();

    AppSettings* settings_;
    QList<Action> actions_;
    QSet<QString> global_failed_;
    int revision_ = 0;
    bool capturing_ = false;
};
