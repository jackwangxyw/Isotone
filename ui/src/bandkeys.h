// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The selected band's keys (Settings, Shortcuts, "Selected band"), as the
// prototype's keydown handler: Up and Down gain by 0.1 dB, Left and Right
// frequency by a 48th of an octave, [ and ] width by 5%, Shift six steps.
//
// Each press, and each repeat of a held key, is a live edit (EqSession's apply
// path: IsoAPO outputs hear every step); the key's release commits once, so an
// Equalizer APO output is written once per hold (ui-spec.md, "Compat devices
// hear an edit when it is committed"). Losing the window's focus while a key is
// held commits too.
//
// Filters its window's key events: nothing happens while a text field has
// focus, or with Ctrl, Alt or Win held (those are app shortcuts).

#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include "eqsession.h"

class BandKeys : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(EqSession* session MEMBER session_ NOTIFY sessionChanged)
    Q_PROPERTY(bool active MEMBER active_ NOTIFY activeChanged)

public:
    explicit BandKeys(QQuickItem* parent = nullptr);

    static constexpr double kGainStepDb = 0.1;
    static constexpr double kFrequencyStepOctaves = 1.0 / 48.0;
    static constexpr double kWidthStep = 1.05;
    static constexpr int kCoarse = 6;

signals:
    void sessionChanged();
    void activeChanged();
    // A held key's edit was committed.
    void editFinished();

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void finish();

    QPointer<EqSession> session_;
    QPointer<QQuickWindow> window_;
    bool active_ = true;
    int held_ = 0;   // the key being held, 0 for none
};
