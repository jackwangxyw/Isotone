// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The tray icon's menu (the prototype's "Tray menu" screen): EQ and Mute,
// checked, with their shortcut keys; Output and Preset submenus, the current
// one checked; Open Isotone; Quit. Follows the session, outputs, presets and
// shortcut bindings as they change. The tooltip is "Isotone", then output ·
// preset. TrayIcon in main.cpp shows it.

#pragma once

#include <QObject>
#include <QString>

#include <memory>

class EqSession;
class Outputs;
class Presets;
class QAction;
class QMenu;
class QSystemTrayIcon;
class ShortcutRegistry;

class TrayMenu : public QObject {
    Q_OBJECT

public:
    TrayMenu(EqSession* session, Outputs* outputs, Presets* presets, ShortcutRegistry* shortcuts, QObject* parent = nullptr);
    ~TrayMenu() override;

    QMenu* menu() const { return menu_.get(); }
    QString tooltip() const;

    // The tray icon with this menu: left click is openRequested.
    void attach(QSystemTrayIcon* icon);

signals:
    void openRequested();
    void quitRequested();

private:
    void updateChecks();
    void updateShortcutText();
    void rebuildOutputs();
    void rebuildPresets();
    void updateCurrent();
    void updateTooltip();

    EqSession* session_;
    Outputs* outputs_;
    Presets* presets_;
    ShortcutRegistry* shortcuts_;
    std::unique_ptr<QMenu> menu_;
    QMenu* output_menu_ = nullptr;
    QMenu* preset_menu_ = nullptr;
    QAction* eq_ = nullptr;
    QAction* mute_ = nullptr;
    QSystemTrayIcon* icon_ = nullptr;
};
