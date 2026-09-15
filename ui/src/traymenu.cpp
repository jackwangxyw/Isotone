// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "traymenu.h"

#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>

#include "eqsession.h"
#include "outputs.h"
#include "presets.h"
#include "shortcutregistry.h"

TrayMenu::TrayMenu(EqSession* session, Outputs* outputs, Presets* presets, ShortcutRegistry* shortcuts, QObject* parent)
    : QObject(parent), session_(session), outputs_(outputs), presets_(presets), shortcuts_(shortcuts), menu_(std::make_unique<QMenu>()) {
    eq_ = menu_->addAction(QString());
    eq_->setCheckable(true);
    connect(eq_, &QAction::triggered, this, [this] { session_->setEqOn(!session_->eqOn()); updateChecks(); });
    mute_ = menu_->addAction(QString());
    mute_->setCheckable(true);
    connect(mute_, &QAction::triggered, this, [this] { session_->setMuted(!session_->muted()); updateChecks(); });
    menu_->addSeparator();
    output_menu_ = menu_->addMenu(QStringLiteral("Output"));
    preset_menu_ = menu_->addMenu(QStringLiteral("Preset"));
    menu_->addSeparator();
    connect(menu_->addAction(QStringLiteral("Open Isotone")), &QAction::triggered, this, &TrayMenu::openRequested);
    connect(menu_->addAction(QStringLiteral("Quit")), &QAction::triggered, this, &TrayMenu::quitRequested);

    connect(session_, &EqSession::stateChanged, this, &TrayMenu::updateChecks);
    connect(shortcuts_, &ShortcutRegistry::changed, this, &TrayMenu::updateShortcutText);
    // Rebuilt later, not inside a signal that one of the old items may have started.
    connect(outputs_, &Outputs::countChanged, this, &TrayMenu::rebuildOutputs, Qt::QueuedConnection);
    connect(outputs_, &Outputs::currentChanged, this, &TrayMenu::updateCurrent);
    connect(presets_, &Presets::countChanged, this, &TrayMenu::rebuildPresets, Qt::QueuedConnection);
    connect(presets_, &Presets::currentChanged, this, &TrayMenu::updateCurrent);

    updateChecks();
    updateShortcutText();
    rebuildOutputs();
    rebuildPresets();
}

TrayMenu::~TrayMenu() = default;

void TrayMenu::attach(QSystemTrayIcon* icon) {
    icon_ = icon;
    icon_->setContextMenu(menu_.get());
    connect(icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) emit openRequested();
    });
    updateTooltip();
}

QString TrayMenu::tooltip() const {
    return QStringLiteral("Isotone\n%1 · %2").arg(outputs_->currentName(), presets_->currentName());
}

void TrayMenu::updateTooltip() {
    if (icon_) icon_->setToolTip(tooltip());
}

void TrayMenu::updateChecks() {
    eq_->setChecked(session_->eqOn());
    mute_->setChecked(session_->muted());
}

void TrayMenu::updateShortcutText() {
    // A menu shows the text after a tab as the shortcut column.
    const auto text = [this](const QString& label, const char* id) {
        const QString keys = shortcuts_->nativeText(QString::fromLatin1(id));
        return keys.isEmpty() ? label : label + QLatin1Char('\t') + keys;
    };
    eq_->setText(text(QStringLiteral("EQ"), "eq"));
    mute_->setText(text(QStringLiteral("Mute"), "mute"));
}

void TrayMenu::rebuildOutputs() {
    output_menu_->clear();
    for (int row = 0; row < outputs_->rowCount(); ++row) {
        QAction* a = output_menu_->addAction(outputs_->data(outputs_->index(row), Outputs::NameRole).toString());
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [this, row] {
            outputs_->select(row);
            updateCurrent();
        });
    }
    output_menu_->menuAction()->setEnabled(outputs_->rowCount() > 0);
    updateCurrent();
}

void TrayMenu::rebuildPresets() {
    preset_menu_->clear();
    const QStringList names = presets_->names();
    for (const QString& name : names) {
        QAction* a = preset_menu_->addAction(name);
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [this, name] {
            presets_->load(name);
            updateCurrent();
        });
    }
    preset_menu_->menuAction()->setEnabled(!names.isEmpty());
    updateCurrent();
}

void TrayMenu::updateCurrent() {
    const QList<QAction*> outputs = output_menu_->actions();
    for (int row = 0; row < outputs.size(); ++row) outputs[row]->setChecked(row == outputs_->currentRow());
    for (QAction* a : preset_menu_->actions()) a->setChecked(a->text() == presets_->currentName());
    updateTooltip();
}
