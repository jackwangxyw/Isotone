// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "globalhotkeys.h"

#include <QKeySequence>

#include "shortcutregistry.h"

namespace {

constexpr wchar_t kWindowClass[] = L"IsotoneGlobalHotkeys";

UINT virtual_key(Qt::Key key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return static_cast<UINT>('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    switch (key) {
        case Qt::Key_Left: return VK_LEFT;
        case Qt::Key_Right: return VK_RIGHT;
        case Qt::Key_Up: return VK_UP;
        case Qt::Key_Down: return VK_DOWN;
        case Qt::Key_Delete: return VK_DELETE;
        case Qt::Key_Insert: return VK_INSERT;
        case Qt::Key_Home: return VK_HOME;
        case Qt::Key_End: return VK_END;
        case Qt::Key_PageUp: return VK_PRIOR;
        case Qt::Key_PageDown: return VK_NEXT;
        case Qt::Key_Space: return VK_SPACE;
        case Qt::Key_Return:
        case Qt::Key_Enter: return VK_RETURN;
        case Qt::Key_Tab: return VK_TAB;
        case Qt::Key_Backspace: return VK_BACK;
        case Qt::Key_Pause: return VK_PAUSE;
        case Qt::Key_VolumeMute: return VK_VOLUME_MUTE;
        case Qt::Key_VolumeDown: return VK_VOLUME_DOWN;
        case Qt::Key_VolumeUp: return VK_VOLUME_UP;
        case Qt::Key_MediaPlay:
        case Qt::Key_MediaTogglePlayPause: return VK_MEDIA_PLAY_PAUSE;
        case Qt::Key_MediaNext: return VK_MEDIA_NEXT_TRACK;
        case Qt::Key_MediaPrevious: return VK_MEDIA_PREV_TRACK;
        default: break;
    }
    // A character key: the virtual key the keyboard layout gives it.
    if (key > 0x20 && key < 0x10000) {
        const SHORT scan = VkKeyScanW(static_cast<wchar_t>(key));
        if (scan != -1) return static_cast<UINT>(scan & 0xff);
    }
    return 0;
}

}  // namespace

GlobalHotkeys::GlobalHotkeys(ShortcutRegistry* registry, QObject* parent) : QObject(parent), registry_(registry) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &GlobalHotkeys::windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);   // fails harmlessly when a previous instance registered it
    hwnd_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    connect(registry_, &ShortcutRegistry::bindingsChanged, this, &GlobalHotkeys::apply);
    // While the Shortcuts page waits for keys, a global hotkey would take them first.
    connect(registry_, &ShortcutRegistry::capturingChanged, this, [this] {
        if (registry_->capturing())
            unregisterAll();
        else
            apply();
    });
    apply();
}

GlobalHotkeys::~GlobalHotkeys() {
    unregisterAll();
    DestroyWindow(hwnd_);
}

bool GlobalHotkeys::toNative(const QString& sequence, UINT* modifiers, UINT* vk) {
    const QKeySequence ks = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (ks.isEmpty()) return false;
    const QKeyCombination c = ks[0];
    *vk = virtual_key(c.key());
    if (*vk == 0) return false;
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    *modifiers = MOD_NOREPEAT;
    if (m & Qt::ControlModifier) *modifiers |= MOD_CONTROL;
    if (m & Qt::AltModifier) *modifiers |= MOD_ALT;
    if (m & Qt::ShiftModifier) *modifiers |= MOD_SHIFT;
    if (m & Qt::MetaModifier) *modifiers |= MOD_WIN;
    return true;
}

void GlobalHotkeys::unregisterAll() {
    for (auto it = ids_.cbegin(); it != ids_.cend(); ++it) UnregisterHotKey(hwnd_, it.key());
    ids_.clear();
}

void GlobalHotkeys::apply() {
    unregisterAll();
    if (registry_->capturing()) return;
    int next = 1;
    for (const QString& id : registry_->ids(QStringLiteral("app"))) {
        UINT modifiers = 0, vk = 0;
        if (!registry_->isGlobal(id) || !toNative(registry_->sequence(id), &modifiers, &vk)) {
            registry_->setGlobalFailed(id, false);
            continue;
        }
        const int hotkey = next++;
        const bool ok = RegisterHotKey(hwnd_, hotkey, modifiers, vk) != 0;
        if (ok) ids_.insert(hotkey, id);
        registry_->setGlobalFailed(id, !ok);
    }
}

QStringList GlobalHotkeys::registered() const {
    QStringList out;
    for (const QString& id : registry_->ids(QStringLiteral("app")))
        if (std::find(ids_.cbegin(), ids_.cend(), id) != ids_.cend()) out.push_back(id);
    return out;
}

bool GlobalHotkeys::simulate(const QString& id) {
    for (auto it = ids_.cbegin(); it != ids_.cend(); ++it) {
        if (it.value() != id) continue;
        SendMessageW(hwnd_, WM_HOTKEY, static_cast<WPARAM>(it.key()), 0);
        return true;
    }
    return false;
}

LRESULT CALLBACK GlobalHotkeys::windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_HOTKEY) {
        auto* self = reinterpret_cast<GlobalHotkeys*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        const QString id = self ? self->ids_.value(static_cast<int>(wparam)) : QString();
        if (!id.isEmpty()) self->registry_->activate(id);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
