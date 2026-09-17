// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "shortcutregistry.h"

#include <QQmlEngine>

#include "appsettings.h"

namespace {

QString normalised(const QString& sequence) {
    const QKeySequence ks = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    return ks.isEmpty() ? QString() : QKeySequence(ks[0]).toString(QKeySequence::PortableText);
}

QString key_cap(Qt::Key key) {
    switch (key) {
        case Qt::Key_Left: return QStringLiteral("←");
        case Qt::Key_Right: return QStringLiteral("→");
        case Qt::Key_Up: return QStringLiteral("↑");
        case Qt::Key_Down: return QStringLiteral("↓");
        case Qt::Key_Delete: return QStringLiteral("Delete");
        default: return QKeySequence(key).toString(QKeySequence::NativeText);
    }
}

}  // namespace

ShortcutRegistry::ShortcutRegistry(AppSettings* settings, QObject* parent) : QObject(parent), settings_(settings) {
    const auto app = [this](const char* id, const char* label, QKeySequence keys, bool global) {
        actions_.push_back({QString::fromLatin1(id), QString::fromUtf8(label), QStringLiteral("app"), keys, global, true, {}});
    };
    app("eq", "EQ on / off", QKeySequence(Qt::CTRL | Qt::Key_E), true);
    app("mute", "Mute", QKeySequence(Qt::CTRL | Qt::Key_M), true);
    app("nextPreset", "Next preset", QKeySequence(Qt::CTRL | Qt::Key_Right), true);
    app("previousPreset", "Previous preset", QKeySequence(Qt::CTRL | Qt::Key_Left), true);
    app("savePreset", "Save preset", QKeySequence(Qt::CTRL | Qt::Key_S), false);
    app("undo", "Undo", QKeySequence(Qt::CTRL | Qt::Key_Z), false);
    app("redo", "Redo", QKeySequence(Qt::CTRL | Qt::Key_Y), false);
    const auto band = [this](const char* id, const char* label, QStringList caps) {
        actions_.push_back({QString::fromLatin1(id), QString::fromUtf8(label), QStringLiteral("band"), {}, false, false, caps});
    };
    band("frequency", "Frequency", {key_cap(Qt::Key_Left), key_cap(Qt::Key_Right)});
    band("gain", "Gain", {key_cap(Qt::Key_Up), key_cap(Qt::Key_Down)});
    band("q", "Q", {QStringLiteral("["), QStringLiteral("]")});
    band("coarse", "Coarse steps", {QStringLiteral("Shift")});
    actions_.push_back({QStringLiteral("delete"), QStringLiteral("Delete"), QStringLiteral("band"), QKeySequence(Qt::Key_Delete), false, true, {}});
}

ShortcutRegistry* ShortcutRegistry::create(QQmlEngine* qml, QJSEngine*) {
    return new ShortcutRegistry(qml->singletonInstance<AppSettings*>("Isotone", "AppSettings"));
}

const ShortcutRegistry::Action* ShortcutRegistry::find(const QString& id) const {
    for (const Action& a : actions_)
        if (a.id == id) return &a;
    return nullptr;
}

void ShortcutRegistry::bump() {
    ++revision_;
    emit changed();
}

void ShortcutRegistry::setCapturing(bool on) {
    if (on == capturing_) return;
    capturing_ = on;
    emit capturingChanged();
}

QStringList ShortcutRegistry::ids(const QString& section) const {
    QStringList out;
    for (const Action& a : actions_)
        if (a.section == section) out.push_back(a.id);
    return out;
}

QString ShortcutRegistry::label(const QString& id) const {
    const Action* a = find(id);
    return a ? a->label : QString();
}

bool ShortcutRegistry::rebindable(const QString& id) const {
    const Action* a = find(id);
    return a && a->rebindable;
}

bool ShortcutRegistry::globalCapable(const QString& id) const {
    const Action* a = find(id);
    return a && a->global_capable;
}

QString ShortcutRegistry::sequence(const QString& id) const {
    const Action* a = find(id);
    if (!a || !a->rebindable) return {};
    const QString fallback = a->defaults.toString(QKeySequence::PortableText);
    return normalised(settings_->value(QStringLiteral("shortcuts/") + id, fallback).toString());
}

QString ShortcutRegistry::nativeText(const QString& id) const {
    return QKeySequence::fromString(sequence(id), QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

QStringList ShortcutRegistry::keyCaps(const QString& id) const {
    const Action* a = find(id);
    if (!a) return {};
    return a->rebindable ? keyCapsFor(sequence(id)) : a->fixed_caps;
}

QStringList ShortcutRegistry::keyCapsFor(const QString& sequence) const {
    const QKeySequence ks = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (ks.isEmpty()) return {};
    const QKeyCombination c = ks[0];
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    QStringList caps;
    if (m & Qt::ControlModifier) caps << QStringLiteral("Ctrl");
    if (m & Qt::AltModifier) caps << QStringLiteral("Alt");
    if (m & Qt::ShiftModifier) caps << QStringLiteral("Shift");
    if (m & Qt::MetaModifier) caps << QStringLiteral("Win");
    caps << key_cap(c.key());
    return caps;
}

QString ShortcutRegistry::sequenceFor(int key, int modifiers) const {
    switch (key) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_CapsLock:
        case Qt::Key_Escape:
        case Qt::Key_unknown:
        case 0: return {};
        default: break;
    }
    const Qt::KeyboardModifiers m =
        Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(m, static_cast<Qt::Key>(key))).toString(QKeySequence::PortableText);
}

bool ShortcutRegistry::isGlobal(const QString& id) const {
    const Action* a = find(id);
    // On unless turned off (owner, 2026-09-16).
    return a && a->global_capable && settings_->value(QStringLiteral("shortcuts/%1/global").arg(id), true).toBool();
}

void ShortcutRegistry::setGlobal(const QString& id, bool on) {
    const Action* a = find(id);
    if (!a || !a->global_capable || isGlobal(id) == on) return;
    settings_->setValue(QStringLiteral("shortcuts/%1/global").arg(id), on);
    if (!on) global_failed_.remove(id);
    bump();
    emit bindingsChanged();
}

bool ShortcutRegistry::globalFailed(const QString& id) const { return global_failed_.contains(id); }

void ShortcutRegistry::setGlobalFailed(const QString& id, bool failed) {
    if (failed == global_failed_.contains(id)) return;
    if (failed)
        global_failed_.insert(id);
    else
        global_failed_.remove(id);
    bump();
}

QString ShortcutRegistry::conflict(const QString& id, const QString& sequence) const {
    const QString wanted = normalised(sequence);
    if (wanted.isEmpty()) return {};
    const QKeyCombination c = QKeySequence::fromString(wanted, QKeySequence::PortableText)[0];
    // The selected band's keys, alone or with Shift.
    if ((c.keyboardModifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
        switch (c.key()) {
            case Qt::Key_Left:
            case Qt::Key_Right: return QStringLiteral("frequency");
            case Qt::Key_Up:
            case Qt::Key_Down: return QStringLiteral("gain");
            case Qt::Key_BracketLeft:
            case Qt::Key_BracketRight:
            case Qt::Key_BraceLeft:
            case Qt::Key_BraceRight: return QStringLiteral("q");
            default: break;
        }
    }
    for (const Action& a : actions_) {
        if (a.id != id && a.rebindable && this->sequence(a.id) == wanted) return a.id;
    }
    return {};
}

void ShortcutRegistry::store(const QString& id, const QString& sequence) {
    settings_->setValue(QStringLiteral("shortcuts/") + id, normalised(sequence));
}

bool ShortcutRegistry::rebind(const QString& id, const QString& sequence) {
    if (!rebindable(id) || normalised(sequence).isEmpty() || !conflict(id, sequence).isEmpty()) return false;
    store(id, sequence);
    bump();
    emit bindingsChanged();
    return true;
}

bool ShortcutRegistry::replace(const QString& id, const QString& sequence) {
    if (!rebindable(id) || normalised(sequence).isEmpty()) return false;
    const QString other = conflict(id, sequence);
    if (!other.isEmpty()) {
        if (!rebindable(other)) return false;
        store(other, QString());
    }
    store(id, sequence);
    bump();
    emit bindingsChanged();
    return true;
}

void ShortcutRegistry::activate(const QString& id) { emit activated(id); }
