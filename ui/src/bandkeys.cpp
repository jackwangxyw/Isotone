// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "bandkeys.h"

#include <QKeyEvent>
#include <QQuickWindow>

#include <cmath>

#include "eqsession.h"

namespace {

// Shift+[ and Shift+] arrive as { and }: the same keys.
int base_key(int key) {
    if (key == Qt::Key_BraceLeft) return Qt::Key_BracketLeft;
    if (key == Qt::Key_BraceRight) return Qt::Key_BracketRight;
    return key;
}

}  // namespace

BandKeys::BandKeys(QQuickItem* parent) : QQuickItem(parent) {}

void BandKeys::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change != ItemSceneChange) return;
    if (window_) window_->removeEventFilter(this);
    window_ = data.window;
    if (window_) window_->installEventFilter(this);
}

void BandKeys::finish() {
    held_ = 0;
    if (session_) session_->finishEdit();
    emit editFinished();
}

bool BandKeys::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_) return false;
    if ((event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate) && held_ != 0) {
        finish();
        return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return false;
    const auto* key = static_cast<QKeyEvent*>(event);
    const int k = base_key(key->key());

    if (event->type() == QEvent::KeyRelease) {
        if (!key->isAutoRepeat() && held_ != 0 && k == held_) finish();
        return false;
    }

    if (!active_ || !session_) return false;
    if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    if (k != Qt::Key_Up && k != Qt::Key_Down && k != Qt::Key_Left && k != Qt::Key_Right && k != Qt::Key_BracketLeft &&
        k != Qt::Key_BracketRight) {
        return false;
    }
    const QQuickItem* focus = window_->activeFocusItem();
    if (focus && focus->flags().testFlag(QQuickItem::ItemAcceptsInputMethod)) return false;
    const int row = session_->selectedRow();
    const isotone::Band* b = session_->bandAt(row);
    if (b == nullptr) return false;

    const int steps = key->modifiers() & Qt::ShiftModifier ? kCoarse : 1;
    switch (k) {
        case Qt::Key_Up: session_->setGain(row, b->gain_db + kGainStepDb * steps); break;
        case Qt::Key_Down: session_->setGain(row, b->gain_db - kGainStepDb * steps); break;
        case Qt::Key_Right: session_->setFrequency(row, b->fc * std::pow(2.0, kFrequencyStepOctaves * steps)); break;
        case Qt::Key_Left: session_->setFrequency(row, b->fc / std::pow(2.0, kFrequencyStepOctaves * steps)); break;
        case Qt::Key_BracketRight: session_->setWidth(row, b->width * std::pow(kWidthStep, steps), false); break;
        case Qt::Key_BracketLeft: session_->setWidth(row, b->width / std::pow(kWidthStep, steps), false); break;
        default: break;
    }
    held_ = k;
    return true;
}
