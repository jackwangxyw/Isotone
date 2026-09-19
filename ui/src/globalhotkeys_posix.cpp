// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Global shortcuts on Linux (globalhotkeys.h says which mechanism, when).

#include "globalhotkeys.h"

#include <QAbstractNativeEventFilter>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QGuiApplication>
#include <QKeySequence>
#include <QRandomGenerator>
#include <QVariantMap>
#include <QtGui/qguiapplication_platform.h>

#include <X11/Xlib.h>
#include <X11/XF86keysym.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

#include "shortcutregistry.h"

// ---------------------------------------------------------------------------
// The GlobalShortcuts portal
// (https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)

struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(PortalShortcut)

QDBusArgument& operator<<(QDBusArgument& a, const PortalShortcut& s) {
    a.beginStructure();
    a << s.id << s.properties;
    a.endStructure();
    return a;
}

const QDBusArgument& operator>>(const QDBusArgument& a, PortalShortcut& s) {
    a.beginStructure();
    a >> s.id >> s.properties;
    a.endStructure();
    return a;
}

namespace {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kShortcutsInterface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

QString token() { return QStringLiteral("isotone%1").arg(QRandomGenerator::global()->generate()); }

}  // namespace

class GlobalShortcutsPortal : public QObject {
    Q_OBJECT

public:
    explicit GlobalShortcutsPortal(QObject* parent = nullptr) : QObject(parent) {
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<QList<PortalShortcut>>();
        bus_.connect(kPortalService, kPortalPath, kShortcutsInterface, QStringLiteral("Activated"), this,
                     SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    }
    ~GlobalShortcutsPortal() override { close(); }

    // The portal answers for its version when the desktop implements it.
    static bool available() {
        QDBusMessage get = QDBusMessage::createMethodCall(kPortalService, kPortalPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("Get"));
        get << kShortcutsInterface << QStringLiteral("version");
        const QDBusMessage reply = QDBusConnection::sessionBus().call(get, QDBus::Block, 3000);
        return reply.type() == QDBusMessage::ReplyMessage;
    }

    // A new session with these shortcuts: a session binds once.
    void bind(const QList<PortalShortcut>& shortcuts) {
        close();
        pending_ = shortcuts;
        if (pending_.isEmpty()) {
            emit bound({});
            return;
        }
        const QString handle = token();
        watchResponse(handle, SLOT(onCreateResponse(uint, QVariantMap)));
        QDBusMessage create = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsInterface, QStringLiteral("CreateSession"));
        create << QVariantMap{{QStringLiteral("handle_token"), handle}, {QStringLiteral("session_handle_token"), token()}};
        bus_.asyncCall(create);
    }

    void close() {
        if (session_.isEmpty()) return;
        bus_.asyncCall(QDBusMessage::createMethodCall(kPortalService, session_, QStringLiteral("org.freedesktop.portal.Session"),
                                                      QStringLiteral("Close")));
        session_.clear();
    }

signals:
    void bound(const QStringList& ids);
    void activated(const QString& id);

private slots:
    void onCreateResponse(uint response, const QVariantMap& results) {
        dropResponse();
        if (response != 0) {
            emit bound({});
            return;
        }
        session_ = results.value(QStringLiteral("session_handle")).toString();
        const QString handle = token();
        watchResponse(handle, SLOT(onBindResponse(uint, QVariantMap)));
        QDBusMessage bind = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kShortcutsInterface, QStringLiteral("BindShortcuts"));
        bind << QVariant::fromValue(QDBusObjectPath(session_)) << QVariant::fromValue(pending_) << QString()
             << QVariantMap{{QStringLiteral("handle_token"), handle}};
        bus_.asyncCall(bind);
    }

    void onBindResponse(uint response, const QVariantMap& results) {
        dropResponse();
        QStringList ids;
        if (response == 0) {
            QList<PortalShortcut> list;
            results.value(QStringLiteral("shortcuts")).value<QDBusArgument>() >> list;
            for (const PortalShortcut& s : list) ids << s.id;
        }
        emit bound(ids);
    }

    void onActivated(const QDBusObjectPath& session, const QString& id, qulonglong, const QVariantMap&) {
        if (session.path() == session_) emit activated(id);
    }

private:
    // The Request object's path is known before the call, so the Response
    // cannot arrive before anything listens for it.
    void watchResponse(const QString& handle, const char* slot) {
        QString sender = bus_.baseService().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        request_ = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handle);
        slot_ = slot;
        bus_.connect(kPortalService, request_, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this, slot);
    }
    void dropResponse() {
        bus_.disconnect(kPortalService, request_, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
                        slot_.constData());
    }

    QDBusConnection bus_ = QDBusConnection::sessionBus();
    QString session_;
    QString request_;
    QByteArray slot_;
    QList<PortalShortcut> pending_;
};

// ---------------------------------------------------------------------------
// X11

class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyFilter(GlobalHotkeys* owner) : owner_(owner) {}
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "xcb_generic_event_t") return false;
        const auto* ev = static_cast<const xcb_generic_event_t*>(message);
        const uint8_t kind = ev->response_type & 0x7f;
        if (kind != XCB_KEY_PRESS && kind != XCB_KEY_RELEASE) return false;
        const auto* key = static_cast<const xcb_key_press_event_t*>(message);
        return owner_->keyEvent(key->detail, key->state, kind == XCB_KEY_PRESS);
    }

private:
    GlobalHotkeys* owner_;
};

namespace {

// The modifiers a binding is made of; Caps Lock and Num Lock (Mod2) are grabbed either way.
constexpr unsigned kBindingMask = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;
constexpr unsigned kLockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

unsigned long keysym_for(Qt::Key key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return XK_a + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return XK_0 + (key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return XK_F1 + (key - Qt::Key_F1);
    switch (key) {
        case Qt::Key_Left: return XK_Left;
        case Qt::Key_Right: return XK_Right;
        case Qt::Key_Up: return XK_Up;
        case Qt::Key_Down: return XK_Down;
        case Qt::Key_Delete: return XK_Delete;
        case Qt::Key_Insert: return XK_Insert;
        case Qt::Key_Home: return XK_Home;
        case Qt::Key_End: return XK_End;
        case Qt::Key_PageUp: return XK_Prior;
        case Qt::Key_PageDown: return XK_Next;
        case Qt::Key_Space: return XK_space;
        case Qt::Key_Return: return XK_Return;
        case Qt::Key_Enter: return XK_KP_Enter;
        case Qt::Key_Tab: return XK_Tab;
        case Qt::Key_Backspace: return XK_BackSpace;
        case Qt::Key_Pause: return XK_Pause;
        case Qt::Key_VolumeMute: return XF86XK_AudioMute;
        case Qt::Key_VolumeDown: return XF86XK_AudioLowerVolume;
        case Qt::Key_VolumeUp: return XF86XK_AudioRaiseVolume;
        case Qt::Key_MediaPlay:
        case Qt::Key_MediaTogglePlayPause: return XF86XK_AudioPlay;
        case Qt::Key_MediaNext: return XF86XK_AudioNext;
        case Qt::Key_MediaPrevious: return XF86XK_AudioPrev;
        default: break;
    }
    // A character: Latin-1 keysyms are their code points; the rest are 0x01000000 + the code point.
    if (key >= 0x20 && key <= 0xff) return static_cast<unsigned long>(QChar(static_cast<char16_t>(key)).toLower().unicode());
    if (key > 0xff && key < 0x110000) return 0x01000000ul | static_cast<unsigned long>(key);
    return 0;
}

}  // namespace

bool GlobalHotkeys::toKeysym(const QString& sequence, unsigned* modifiers, unsigned long* keysym) {
    const QKeySequence ks = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (ks.isEmpty()) return false;
    const QKeyCombination c = ks[0];
    *keysym = keysym_for(c.key());
    if (*keysym == 0) return false;
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    *modifiers = 0;
    if (m & Qt::ControlModifier) *modifiers |= ControlMask;
    if (m & Qt::AltModifier) *modifiers |= Mod1Mask;
    if (m & Qt::ShiftModifier) *modifiers |= ShiftMask;
    if (m & Qt::MetaModifier) *modifiers |= Mod4Mask;
    return true;
}

QString GlobalHotkeys::toPortalTrigger(const QString& sequence) {
    unsigned modifiers = 0;
    unsigned long keysym = 0;
    if (!toKeysym(sequence, &modifiers, &keysym)) return QString();
    const char* name = XKeysymToString(keysym);
    if (name == nullptr) return QString();
    QStringList parts;
    if (modifiers & ControlMask) parts << QStringLiteral("CTRL");
    if (modifiers & Mod1Mask) parts << QStringLiteral("ALT");
    if (modifiers & ShiftMask) parts << QStringLiteral("SHIFT");
    if (modifiers & Mod4Mask) parts << QStringLiteral("LOGO");
    parts << QString::fromLatin1(name);
    return parts.join(QLatin1Char('+'));
}

GlobalHotkeys::GlobalHotkeys(ShortcutRegistry* registry, QObject* parent) : QObject(parent), registry_(registry) {
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    if (x11 && x11->connection() && x11->display()) {
        mechanism_ = QStringLiteral("x11");
        filter_ = std::make_unique<HotkeyFilter>(this);
        qGuiApp->installNativeEventFilter(filter_.get());
    } else if (GlobalShortcutsPortal::available()) {
        mechanism_ = QStringLiteral("portal");
        portal_ = new GlobalShortcutsPortal(this);
        connect(portal_, &GlobalShortcutsPortal::bound, this, [this](const QStringList& ids) {
            bound_ = ids;
            for (const QString& id : registry_->ids(QStringLiteral("app")))
                if (registry_->isGlobal(id) && !registry_->sequence(id).isEmpty()) registry_->setGlobalFailed(id, !ids.contains(id));
        });
        connect(portal_, &GlobalShortcutsPortal::activated, this, [this](const QString& id) {
            if (bound_.contains(id)) registry_->activate(id);
        });
    }
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
    if (filter_ && qGuiApp) qGuiApp->removeNativeEventFilter(filter_.get());
}

void GlobalHotkeys::unregisterAll() {
    if (mechanism_ == QLatin1String("x11") && !grabs_.isEmpty()) {
        auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        xcb_connection_t* c = x11->connection();
        const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
        for (const Grab& g : grabs_)
            for (unsigned extra : kLockVariants)
                xcb_ungrab_key(c, static_cast<xcb_keycode_t>(g.keycode), root, static_cast<uint16_t>(g.modifiers | extra));
        xcb_flush(c);
    }
    grabs_.clear();
    if (portal_) portal_->close();
    bound_.clear();
}

void GlobalHotkeys::apply() {
    unregisterAll();
    if (registry_->capturing()) return;
    if (mechanism_ == QLatin1String("x11"))
        grabX11();
    else if (mechanism_ == QLatin1String("portal"))
        bindPortal();
    else
        for (const QString& id : registry_->ids(QStringLiteral("app")))
            registry_->setGlobalFailed(id, registry_->isGlobal(id) && !registry_->sequence(id).isEmpty());
}

void GlobalHotkeys::grabX11() {
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    xcb_connection_t* c = x11->connection();
    Display* display = x11->display();
    const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    for (const QString& id : registry_->ids(QStringLiteral("app"))) {
        unsigned modifiers = 0;
        unsigned long keysym = 0;
        if (!registry_->isGlobal(id) || !toKeysym(registry_->sequence(id), &modifiers, &keysym)) {
            registry_->setGlobalFailed(id, false);
            continue;
        }
        const unsigned keycode = XKeysymToKeycode(display, keysym);
        bool ok = keycode != 0;
        int granted = 0;
        for (unsigned extra : kLockVariants) {
            if (!ok) break;
            const xcb_void_cookie_t cookie = xcb_grab_key_checked(c, 1, root, static_cast<uint16_t>(modifiers | extra),
                                                                  static_cast<xcb_keycode_t>(keycode), XCB_GRAB_MODE_ASYNC,
                                                                  XCB_GRAB_MODE_ASYNC);
            // BadAccess: another client holds these keys.
            if (xcb_generic_error_t* error = xcb_request_check(c, cookie)) {
                free(error);
                ok = false;
            } else {
                ++granted;
            }
        }
        if (ok) {
            grabs_.append(Grab{id, keycode, modifiers, false});
        } else {
            for (int i = 0; i < granted; ++i)
                xcb_ungrab_key(c, static_cast<xcb_keycode_t>(keycode), root, static_cast<uint16_t>(modifiers | kLockVariants[i]));
        }
        registry_->setGlobalFailed(id, !ok);
    }
    xcb_flush(c);
}

void GlobalHotkeys::bindPortal() {
    QList<PortalShortcut> shortcuts;
    for (const QString& id : registry_->ids(QStringLiteral("app"))) {
        const QString trigger = registry_->isGlobal(id) ? toPortalTrigger(registry_->sequence(id)) : QString();
        if (trigger.isEmpty()) {
            registry_->setGlobalFailed(id, false);
            continue;
        }
        shortcuts << PortalShortcut{id, {{QStringLiteral("description"), registry_->label(id)},
                                         {QStringLiteral("preferred_trigger"), trigger}}};
    }
    portal_->bind(shortcuts);
}

bool GlobalHotkeys::keyEvent(unsigned keycode, unsigned state, bool press) {
    for (Grab& g : grabs_) {
        if (g.keycode != keycode || (state & kBindingMask) != g.modifiers) continue;
        if (!press) {
            g.down = false;
            return true;
        }
        // Auto-repeat: once per press, as MOD_NOREPEAT does on Windows.
        if (g.down) return true;
        g.down = true;
        registry_->activate(g.id);
        return true;
    }
    // A release after the modifiers were let go first still ends the press.
    if (!press)
        for (Grab& g : grabs_)
            if (g.keycode == keycode) g.down = false;
    return false;
}

QStringList GlobalHotkeys::registered() const {
    QStringList out;
    for (const QString& id : registry_->ids(QStringLiteral("app"))) {
        bool held = bound_.contains(id);
        for (const Grab& g : grabs_) held |= g.id == id;
        if (held) out.push_back(id);
    }
    return out;
}

bool GlobalHotkeys::simulate(const QString& id) {
    for (const Grab& g : grabs_) {
        if (g.id != id) continue;
        const Grab copy = g;
        keyEvent(copy.keycode, copy.modifiers, true);
        keyEvent(copy.keycode, copy.modifiers, false);
        return true;
    }
    if (bound_.contains(id)) {
        registry_->activate(id);
        return true;
    }
    return false;
}

#include "globalhotkeys_posix.moc"
