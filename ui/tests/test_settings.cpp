// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Settings with Qt, without QML: the graph's ranges, the shortcut registry,
// global hotkeys, the tray menu and the Appearance preview.

#include "doctest.h"

#include <QAction>
#include <QImage>
#include <QPainter>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "apppaths.h"
#include "appsettings.h"
#include "eqsession.h"
#include "globalhotkeys.h"
#include "outputs.h"
#include "presets.h"
#include "previewsession.h"
#include "responsegraph.h"
#include "shortcutregistry.h"
#include "singleinstance.h"
#include "test_rig.h"
#include "traymenu.h"

// Last: Xlib's macros (Bool, Status, None) collide with names in Qt's headers.
#if !defined(_WIN32)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

using namespace isotone;

namespace {

// Settings in a directory of their own, so no test sees another's.
struct Scratch {
    QTemporaryDir dir;
    std::unique_ptr<AppSettings> settings;
    Scratch() {
        AppPaths::setDataDir(dir.path());
        settings = std::make_unique<AppSettings>();
    }
    // A second AppSettings on the same file, as after a restart.
    void reopen() {
        settings.reset();
        settings = std::make_unique<AppSettings>();
    }
};

}  // namespace

TEST_CASE("the graph maps frequency on its range, and gain on each gain range") {
    ResponseGraph graph;
    graph.setSize(QSizeF(1060, 404));
    const double left = graph.plotLeft(), width = graph.plotWidth(), top = graph.plotTop(), height = graph.plotHeight();

    CHECK(graph.minHz() == 20.0);
    CHECK(graph.maxHz() == 20000.0);
    CHECK(graph.xOf(20) == doctest::Approx(left));
    CHECK(graph.xOf(20000) == doctest::Approx(left + width));
    CHECK(graph.xOf(632.4555) == doctest::Approx(left + width / 2).epsilon(1e-6));

    const int before = graph.revision();
    graph.setMinHz(100);
    graph.setMaxHz(10000);
    CHECK(graph.revision() > before);   // handles and the readout re-run
    CHECK(graph.xOf(100) == doctest::Approx(left));
    CHECK(graph.xOf(1000) == doctest::Approx(left + width / 2));
    CHECK(graph.xOf(10000) == doctest::Approx(left + width));
    CHECK(graph.frequencyAt(left + width / 2) == doctest::Approx(1000));
    CHECK(graph.frequencyAt(left - 50) == doctest::Approx(100));
    CHECK(graph.frequencyAt(left + width + 50) == doctest::Approx(10000));

    for (double range : {12.0, 15.0, 24.0}) {
        CAPTURE(range);
        const int r = graph.revision();
        graph.setRangeDb(range);
        CHECK(graph.revision() > r);
        CHECK(graph.yOf(range) == doctest::Approx(top));
        CHECK(graph.yOf(-range) == doctest::Approx(top + height));
        CHECK(graph.yOf(0) == doctest::Approx(top + height / 2));
        CHECK(graph.dbAt(top) == doctest::Approx(range));
        CHECK(graph.dbAt(top + height * 0.25) == doctest::Approx(range / 2));
    }
}

TEST_CASE("the grid and its labels follow any frequency range") {
    // 1, 1.5, 2, 3, 4, 5, 6 and 8 in each decade, near an eighth of a decade apart
    // (squig.link's lines); the labelled ones are the 1s, 2s and 5s.
    auto lines = ResponseGraph::gridLines(20, 20000);
    std::vector<double> hz, major;
    for (const auto& l : lines) {
        hz.push_back(l.hz);
        if (l.major) major.push_back(l.hz);
    }
    CHECK(hz == std::vector<double>{20, 30, 40, 50, 60, 80, 100, 150, 200, 300, 400, 500, 600, 800,
                                    1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 15000, 20000});
    CHECK(major == std::vector<double>{20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000});
    // No gap between neighbours is more than twice another.
    double widest = 0, narrowest = 1;
    for (size_t i = 1; i < hz.size(); ++i) {
        const double gap = std::log10(hz[i] / hz[i - 1]);
        widest = std::max(widest, gap);
        narrowest = std::min(narrowest, gap);
    }
    CHECK(widest / narrowest <= 2.3);
    CHECK(ResponseGraph::labelFrequencies(20, 20000) ==
          std::vector<double>{20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000});

    CHECK(ResponseGraph::labelFrequencies(10, 24000) ==
          std::vector<double>{10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000});
    for (const auto& l : ResponseGraph::gridLines(10, 24000)) CHECK((l.hz >= 10 && l.hz <= 24000));

    // A narrow range still has labels, inside it.
    const auto narrow = ResponseGraph::labelFrequencies(1100, 1900);
    CHECK(narrow.size() >= 2);
    for (double f : narrow) CHECK((f >= 1100 && f <= 1900));
    CHECK(ResponseGraph::gridLines(1100, 1900).size() >= 2);

    const auto octave = ResponseGraph::labelFrequencies(300, 700);
    CHECK(octave.size() >= 3);
}

TEST_CASE("the decades are the strongest separators, the other labels next, the rest faint") {
    // The owner, 2026-09-16: major separators at 100, 1k and 10k, less major ones at
    // every other label, minor ones at every other line.
    ResponseGraph graph;
    graph.setSize(QSizeF(1060, 404));
    const QColor minor(0x20, 0x20, 0x20), major(0x60, 0x60, 0x60), zero(0xa0, 0xa0, 0xa0);
    graph.setProperty("gridMinor", minor);
    graph.setProperty("gridMajor", major);
    graph.setProperty("zeroLine", zero);
    const auto check = [&](double min_hz, double max_hz) {
        CAPTURE(min_hz);
        CAPTURE(max_hz);
        graph.setMinHz(min_hz);
        graph.setMaxHz(max_hz);
        QImage image(1060, 404, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        QPainter painter(&image);
        graph.paint(&painter);
        painter.end();
        // Between the +3 dB and +6 dB lines, clear of the horizontal grid.
        const int y = static_cast<int>(graph.yOf(4.5));
        const auto at = [&](double hz) { return image.pixelColor(static_cast<int>(std::round(graph.xOf(hz))), y); };
        const QColor clear = ResponseGraph::separatorColour(major, zero);
        const std::vector<double> labels = ResponseGraph::labelFrequencies(min_hz, max_hz);
        const auto labelled = [&](double hz) { return std::find(labels.begin(), labels.end(), hz) != labels.end(); };
        int decades = 0;
        for (const auto& l : ResponseGraph::gridLines(min_hz, max_hz)) {
            CAPTURE(l.hz);
            if (ResponseGraph::isDecade(l.hz)) {
                CHECK(at(l.hz) == zero);
                ++decades;
            } else {
                CHECK(at(l.hz) == (labelled(l.hz) ? clear : minor));
            }
        }
        for (double f : labels) {
            CAPTURE(f);
            CHECK(at(f) == (ResponseGraph::isDecade(f) ? zero : clear));
        }
        return decades;
    };
    CHECK(check(20, 20000) == 3);   // 100, 1k, 10k
    check(300, 700);     // labels on lines that are not 1s, 2s or 5s
    check(1100, 1900);   // the narrow steps
    CHECK(check(900, 1100) == 1);   // a decade in the narrow steps
    // A tad less obvious than halfway (owner, 2026-09-16): a quarter of the way from
    // the major grid colour to the zero line's.
    CHECK(ResponseGraph::separatorColour(major, zero) == QColor(0x70, 0x70, 0x70));

    CHECK(ResponseGraph::isDecade(100));
    CHECK(ResponseGraph::isDecade(10000));
    CHECK_FALSE(ResponseGraph::isDecade(20));
    CHECK_FALSE(ResponseGraph::isDecade(2000));
    CHECK_FALSE(ResponseGraph::isDecade(1500));
}

TEST_CASE("an inverted frequency range does not break the mapping") {
    ResponseGraph graph;
    graph.setSize(QSizeF(1060, 404));
    graph.setMinHz(30000);   // on its way to 21000-24000, before maxHz follows
    CHECK(std::isfinite(graph.xOf(1000)));
    CHECK(std::isfinite(graph.frequencyAt(500)));
}

TEST_CASE("shortcut defaults") {
    Scratch s;
    ShortcutRegistry r(s.settings.get());
    CHECK(r.ids(QStringLiteral("app")) == QStringList{"eq", "mute", "nextPreset", "previousPreset", "savePreset", "undo", "redo"});
    CHECK(r.ids(QStringLiteral("band")) == QStringList{"frequency", "gain", "q", "coarse", "delete"});
    CHECK(r.label(QStringLiteral("eq")) == QStringLiteral("EQ on / off"));
    CHECK(r.sequence(QStringLiteral("eq")) == QStringLiteral("Ctrl+E"));
    CHECK(r.sequence(QStringLiteral("mute")) == QStringLiteral("Ctrl+M"));
    CHECK(r.sequence(QStringLiteral("nextPreset")) == QStringLiteral("Ctrl+Right"));
    CHECK(r.sequence(QStringLiteral("previousPreset")) == QStringLiteral("Ctrl+Left"));
    CHECK(r.sequence(QStringLiteral("savePreset")) == QStringLiteral("Ctrl+S"));
    CHECK(r.sequence(QStringLiteral("undo")) == QStringLiteral("Ctrl+Z"));
    CHECK(r.sequence(QStringLiteral("redo")) == QStringLiteral("Ctrl+Y"));
    CHECK(r.sequence(QStringLiteral("delete")) == QStringLiteral("Del"));

    for (const char* id : {"eq", "mute", "nextPreset", "previousPreset"}) {
        CAPTURE(id);
        CHECK(r.globalCapable(QString::fromLatin1(id)));
        // On until turned off (owner, 2026-09-16), as the boards show them.
        CHECK(r.isGlobal(QString::fromLatin1(id)));
    }
    for (const char* id : {"savePreset", "undo", "redo", "delete", "gain"}) {
        CAPTURE(id);
        CHECK_FALSE(r.globalCapable(QString::fromLatin1(id)));
        CHECK_FALSE(r.isGlobal(QString::fromLatin1(id)));
    }
    CHECK(r.rebindable(QStringLiteral("delete")));
    CHECK_FALSE(r.rebindable(QStringLiteral("frequency")));
    CHECK_FALSE(r.rebindable(QStringLiteral("coarse")));

    CHECK(r.keyCaps(QStringLiteral("eq")) == QStringList{"Ctrl", "E"});
    CHECK(r.keyCaps(QStringLiteral("nextPreset")) == QStringList{"Ctrl", QStringLiteral("→")});
    CHECK(r.keyCaps(QStringLiteral("frequency")) == QStringList{QStringLiteral("←"), QStringLiteral("→")});
    CHECK(r.keyCaps(QStringLiteral("gain")) == QStringList{QStringLiteral("↑"), QStringLiteral("↓")});
    CHECK(r.keyCaps(QStringLiteral("q")) == QStringList{"[", "]"});
    CHECK(r.keyCaps(QStringLiteral("coarse")) == QStringList{"Shift"});
    CHECK(r.keyCaps(QStringLiteral("delete")) == QStringList{"Delete"});
    CHECK(r.nativeText(QStringLiteral("eq")) == QStringLiteral("Ctrl+E"));
}

TEST_CASE("a pressed key becomes a sequence; a modifier alone does not") {
    Scratch s;
    ShortcutRegistry r(s.settings.get());
    CHECK(r.sequenceFor(Qt::Key_K, Qt::ControlModifier) == QStringLiteral("Ctrl+K"));
    CHECK(r.sequenceFor(Qt::Key_F5, Qt::ControlModifier | Qt::ShiftModifier) == QStringLiteral("Ctrl+Shift+F5"));
    CHECK(r.sequenceFor(Qt::Key_Control, Qt::ControlModifier).isEmpty());
    CHECK(r.sequenceFor(Qt::Key_Shift, Qt::ShiftModifier).isEmpty());
    CHECK(r.sequenceFor(Qt::Key_Escape, Qt::NoModifier).isEmpty());   // Escape cancels
    CHECK(r.keyCapsFor(QStringLiteral("Ctrl+Alt+Down")) == QStringList{"Ctrl", "Alt", QStringLiteral("↓")});
}

TEST_CASE("rebinding, conflicts and replacing") {
    Scratch s;
    ShortcutRegistry r(s.settings.get());
    QSignalSpy bindings(&r, &ShortcutRegistry::bindingsChanged);

    CHECK(r.conflict(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+K")).isEmpty());
    CHECK(r.rebind(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+K")));
    CHECK(r.sequence(QStringLiteral("nextPreset")) == QStringLiteral("Ctrl+K"));
    CHECK(bindings.count() == 1);

    // Its own keys are no conflict.
    CHECK(r.conflict(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+K")).isEmpty());

    CHECK(r.conflict(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+M")) == QStringLiteral("mute"));
    CHECK_FALSE(r.rebind(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+M")));
    CHECK(r.sequence(QStringLiteral("nextPreset")) == QStringLiteral("Ctrl+K"));
    CHECK(r.sequence(QStringLiteral("mute")) == QStringLiteral("Ctrl+M"));

    // Replace takes the keys; the other action is left without any.
    CHECK(r.replace(QStringLiteral("nextPreset"), QStringLiteral("Ctrl+M")));
    CHECK(r.sequence(QStringLiteral("nextPreset")) == QStringLiteral("Ctrl+M"));
    CHECK(r.sequence(QStringLiteral("mute")).isEmpty());
    CHECK(r.keyCaps(QStringLiteral("mute")).isEmpty());

    // The selected band's fixed keys conflict, with or without Shift, and cannot be replaced.
    CHECK(r.conflict(QStringLiteral("undo"), QStringLiteral("Left")) == QStringLiteral("frequency"));
    CHECK(r.conflict(QStringLiteral("undo"), QStringLiteral("Shift+Up")) == QStringLiteral("gain"));
    CHECK(r.conflict(QStringLiteral("undo"), QStringLiteral("]")) == QStringLiteral("q"));
    CHECK_FALSE(r.replace(QStringLiteral("undo"), QStringLiteral("Left")));
    CHECK(r.sequence(QStringLiteral("undo")) == QStringLiteral("Ctrl+Z"));
    // Nor are the fixed rows rebound.
    CHECK_FALSE(r.rebind(QStringLiteral("frequency"), QStringLiteral("Ctrl+F")));

    CHECK_FALSE(r.rebind(QStringLiteral("nope"), QStringLiteral("Ctrl+J")));
}

TEST_CASE("bindings and Global persist") {
    Scratch s;
    {
        ShortcutRegistry r(s.settings.get());
        r.rebind(QStringLiteral("eq"), QStringLiteral("Ctrl+Shift+E"));
        r.replace(QStringLiteral("redo"), QStringLiteral("Ctrl+Z"));
        r.setGlobal(QStringLiteral("eq"), true);
        r.setGlobal(QStringLiteral("mute"), true);
        r.setGlobal(QStringLiteral("mute"), false);
        r.setGlobal(QStringLiteral("undo"), true);   // not a global action: ignored
    }
    s.reopen();
    ShortcutRegistry again(s.settings.get());
    CHECK(again.sequence(QStringLiteral("eq")) == QStringLiteral("Ctrl+Shift+E"));
    CHECK(again.sequence(QStringLiteral("redo")) == QStringLiteral("Ctrl+Z"));
    CHECK(again.sequence(QStringLiteral("undo")).isEmpty());
    CHECK_FALSE(again.isGlobal(QStringLiteral("mute")));
    CHECK(again.isGlobal(QStringLiteral("eq")));
    CHECK_FALSE(again.isGlobal(QStringLiteral("undo")));
    CHECK(s.settings->value(QStringLiteral("shortcuts/eq")).toString() == QStringLiteral("Ctrl+Shift+E"));
    CHECK(s.settings->value(QStringLiteral("shortcuts/mute/global")).toBool() == false);
}

#if defined(_WIN32)
TEST_CASE("a global hotkey's keys for RegisterHotKey") {
    UINT mods = 0, vk = 0;
    CHECK(GlobalHotkeys::toNative(QStringLiteral("Ctrl+E"), &mods, &vk));
    CHECK(mods == (MOD_CONTROL | MOD_NOREPEAT));
    CHECK(vk == 'E');
    CHECK(GlobalHotkeys::toNative(QStringLiteral("Ctrl+Right"), &mods, &vk));
    CHECK(vk == VK_RIGHT);
    CHECK(GlobalHotkeys::toNative(QStringLiteral("Ctrl+Alt+Shift+F13"), &mods, &vk));
    CHECK(mods == (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT));
    CHECK(vk == VK_F13);
    CHECK(GlobalHotkeys::toNative(QStringLiteral("Ctrl+7"), &mods, &vk));
    CHECK(vk == '7');
    CHECK(GlobalHotkeys::toNative(QStringLiteral("Del"), &mods, &vk));
    CHECK(vk == VK_DELETE);
    CHECK_FALSE(GlobalHotkeys::toNative(QString(), &mods, &vk));
}

TEST_CASE("global hotkeys register for Global actions, report one taken, and activate") {
    // Keys nothing on the desktop is expected to hold.
    const QString rare = QStringLiteral("Ctrl+Alt+Shift+F23");
    Scratch s;
    ShortcutRegistry first(s.settings.get());
    for (const QString& id : first.ids(QStringLiteral("app"))) first.setGlobal(id, false);
    first.rebind(QStringLiteral("mute"), rare);
    first.setGlobal(QStringLiteral("mute"), true);

    auto hotkeys = std::make_unique<GlobalHotkeys>(&first);
    CHECK(hotkeys->registered() == QStringList{"mute"});
    CHECK_FALSE(first.globalFailed(QStringLiteral("mute")));

    QSignalSpy activated(&first, &ShortcutRegistry::activated);
    CHECK(hotkeys->simulate(QStringLiteral("mute")));
    REQUIRE(activated.count() == 1);
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("mute"));

    // Not while the Shortcuts page waits for keys: they must reach the page.
    first.setCapturing(true);
    CHECK(hotkeys->registered().isEmpty());
    // The page binds the keys before it stops capturing: still none until then.
    first.rebind(QStringLiteral("mute"), QStringLiteral("Ctrl+Alt+Shift+F22"));
    CHECK(hotkeys->registered().isEmpty());
    first.rebind(QStringLiteral("mute"), rare);
    first.setCapturing(false);
    CHECK(hotkeys->registered() == QStringList{"mute"});

    // Another holder of the same keys (as another app would be) fails, and says so.
    Scratch s2;
    ShortcutRegistry second(s2.settings.get());
    for (const QString& id : second.ids(QStringLiteral("app"))) second.setGlobal(id, false);
    second.rebind(QStringLiteral("eq"), rare);
    second.setGlobal(QStringLiteral("eq"), true);
    GlobalHotkeys other(&second);
    CHECK(other.registered().isEmpty());
    CHECK(second.globalFailed(QStringLiteral("eq")));

    // Turning Global off unregisters; the keys are free for the other again.
    first.setGlobal(QStringLiteral("mute"), false);
    CHECK(hotkeys->registered().isEmpty());
    other.apply();
    CHECK(other.registered() == QStringList{"eq"});
    CHECK_FALSE(second.globalFailed(QStringLiteral("eq")));

    // Unregistered on destruction.
    first.setGlobal(QStringLiteral("mute"), true);
    CHECK(first.globalFailed(QStringLiteral("mute")));
    other.unregisterAll();
    hotkeys->apply();
    CHECK(hotkeys->registered() == QStringList{"mute"});
    hotkeys.reset();
    other.apply();
    CHECK(other.registered() == QStringList{"eq"});
}
#else
TEST_CASE("a global hotkey's keys for an X grab and for the portal") {
    unsigned mods = 0;
    unsigned long sym = 0;
    CHECK(GlobalHotkeys::toKeysym(QStringLiteral("Ctrl+E"), &mods, &sym));
    CHECK(mods == ControlMask);
    CHECK(sym == XK_e);
    CHECK(GlobalHotkeys::toKeysym(QStringLiteral("Ctrl+Right"), &mods, &sym));
    CHECK(sym == XK_Right);
    CHECK(GlobalHotkeys::toKeysym(QStringLiteral("Ctrl+Alt+Shift+F13"), &mods, &sym));
    CHECK(mods == (ControlMask | Mod1Mask | ShiftMask));
    CHECK(sym == XK_F13);
    CHECK(GlobalHotkeys::toKeysym(QStringLiteral("Ctrl+7"), &mods, &sym));
    CHECK(sym == XK_7);
    CHECK(GlobalHotkeys::toKeysym(QStringLiteral("Meta+Del"), &mods, &sym));
    CHECK(mods == Mod4Mask);
    CHECK(sym == XK_Delete);
    CHECK_FALSE(GlobalHotkeys::toKeysym(QString(), &mods, &sym));

    // The XDG shortcuts specification's form: modifiers in capitals, then the keysym's name.
    CHECK(GlobalHotkeys::toPortalTrigger(QStringLiteral("Ctrl+E")) == QStringLiteral("CTRL+e"));
    CHECK(GlobalHotkeys::toPortalTrigger(QStringLiteral("Ctrl+Alt+Shift+F13")) == QStringLiteral("CTRL+ALT+SHIFT+F13"));
    CHECK(GlobalHotkeys::toPortalTrigger(QStringLiteral("Meta+Right")) == QStringLiteral("LOGO+Right"));
    CHECK(GlobalHotkeys::toPortalTrigger(QString()).isEmpty());
}

TEST_CASE("global hotkeys grab on X11 for Global actions, report one taken, and activate") {
    // Only under an X server: run with QT_QPA_PLATFORM=xcb (ctest does, as ui_model_tests_x11).
    // Keys every X keymap has (F13 and up have no keycode in a standard one) and
    // nothing is expected to hold.
    const QString rare = QStringLiteral("Ctrl+Alt+Shift+Meta+F12");
    Scratch s;
    ShortcutRegistry first(s.settings.get());
    for (const QString& id : first.ids(QStringLiteral("app"))) first.setGlobal(id, false);
    first.rebind(QStringLiteral("mute"), rare);
    first.setGlobal(QStringLiteral("mute"), true);

    auto hotkeys = std::make_unique<GlobalHotkeys>(&first);
    if (hotkeys->mechanism() != QLatin1String("x11")) {
        MESSAGE("not an X11 session; skipped");
        return;
    }
    CHECK(hotkeys->registered() == QStringList{"mute"});
    CHECK_FALSE(first.globalFailed(QStringLiteral("mute")));

    QSignalSpy activated(&first, &ShortcutRegistry::activated);
    CHECK(hotkeys->simulate(QStringLiteral("mute")));
    REQUIRE(activated.count() == 1);
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("mute"));

    // Not while the Shortcuts page waits for keys: they must reach the page.
    first.setCapturing(true);
    CHECK(hotkeys->registered().isEmpty());
    first.setCapturing(false);
    CHECK(hotkeys->registered() == QStringList{"mute"});

    // Another client holding the keys (as another app would) makes the grab fail, and it says so.
    Display* other = XOpenDisplay(nullptr);
    REQUIRE(other != nullptr);
    const Window root = DefaultRootWindow(other);
    const KeyCode code = XKeysymToKeycode(other, XK_F11);
    const unsigned taken = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;
    XGrabKey(other, code, taken, root, 1 /* owner events */, GrabModeAsync, GrabModeAsync);
    XSync(other, 0);
    first.rebind(QStringLiteral("mute"), QStringLiteral("Ctrl+Alt+Shift+Meta+F11"));
    CHECK(hotkeys->registered().isEmpty());
    CHECK(first.globalFailed(QStringLiteral("mute")));

    // Released there, the keys are free again.
    XUngrabKey(other, code, taken, root);
    XSync(other, 0);
    hotkeys->apply();
    CHECK(hotkeys->registered() == QStringList{"mute"});
    CHECK_FALSE(first.globalFailed(QStringLiteral("mute")));

    // Released on destruction: the other client can take them.
    hotkeys.reset();
    static int x_error = 0;
    XErrorHandler before = XSetErrorHandler([](Display*, XErrorEvent* e) -> int {
        x_error = e->error_code;
        return 0;
    });
    XGrabKey(other, code, taken, root, 1 /* owner events */, GrabModeAsync, GrabModeAsync);
    XSync(other, 0);
    CHECK(x_error == 0);
    XUngrabKey(other, code, taken, root);
    XSync(other, 0);
    XSetErrorHandler(before);
    XCloseDisplay(other);
}

namespace {

// processEvents with a time limit returns at once when nothing is queued, so it
// cannot be the wait: the sleep is.
bool wait_until(const std::function<bool()>& done, int ms = 5000) {
    for (int waited = 0; waited < ms && !done(); waited += 10) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

QString read_all(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

}  // namespace

TEST_CASE("global hotkeys bind through the GlobalShortcuts portal, report one refused, and activate") {
    // Only against tests/mock_portal.py, a portal in a process of its own on a
    // session bus of this run's own (ctest runs it so, as ui_model_tests_portal).
    // It binds all but "mute" and presses "eq" once the bind is answered.
    const QString log = qEnvironmentVariable("ISOTONE_MOCK_PORTAL_LOG");
    if (log.isEmpty()) {
        MESSAGE("no mock portal; skipped");
        return;
    }
    Scratch s;
    ShortcutRegistry registry(s.settings.get());
    for (const QString& id : registry.ids(QStringLiteral("app"))) registry.setGlobal(id, false);
    registry.setGlobal(QStringLiteral("eq"), true);
    registry.setGlobal(QStringLiteral("mute"), true);
    QSignalSpy activated(&registry, &ShortcutRegistry::activated);

    auto hotkeys = std::make_unique<GlobalHotkeys>(&registry);
    REQUIRE(hotkeys->mechanism() == QStringLiteral("portal"));
    REQUIRE(wait_until([&] { return hotkeys->registered() == QStringList{"eq"}; }));
    // What the desktop refused is marked; the keys went as the XDG shortcuts specification writes them.
    CHECK_FALSE(registry.globalFailed(QStringLiteral("eq")));
    CHECK(registry.globalFailed(QStringLiteral("mute")));
    CHECK(read_all(log).contains(QStringLiteral("BindShortcuts eq trigger=CTRL+e description=EQ on / off")));
    CHECK(read_all(log).contains(QStringLiteral("BindShortcuts mute trigger=CTRL+m description=Mute")));

    // The desktop's press arrives as Activated on the session: the action runs once.
    REQUIRE(wait_until([&] { return activated.count() == 1; }));
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("eq"));

    // Waiting for keys on the Shortcuts page closes the session.
    registry.setCapturing(true);
    CHECK(hotkeys->registered().isEmpty());
    CHECK(wait_until([&] { return read_all(log).contains(QStringLiteral("Close ")); }));
}
#endif

TEST_CASE("the tray menu") {
    Scratch s;
    ShortcutRegistry shortcuts(s.settings.get());
    EqSession session;
    Outputs outputs;
    // No output list and a place of the test's own: nothing reaches a real output.
    const test_rig::Place place("tray");
    const std::unique_ptr<Presets> owned = place.presets(s.dir.path(), &session, {});
    Presets& presets = *owned;
    TrayMenu tray(&session, &outputs, &presets, &shortcuts);
    QMenu* menu = tray.menu();

    // EQ, Mute, -, Output, Preset, -, Open Isotone, Quit
    const QList<QAction*> items = menu->actions();
    REQUIRE(items.size() == 8);
    // Keys only beside an action whose Global is on (on by default): elsewhere they do nothing.
    CHECK(items[0]->text() == QStringLiteral("EQ\tCtrl+E"));
    CHECK(items[1]->text() == QStringLiteral("Mute\tCtrl+M"));
    shortcuts.setGlobal(QStringLiteral("eq"), false);
    CHECK(items[0]->text() == QStringLiteral("EQ"));
    shortcuts.setGlobal(QStringLiteral("eq"), true);
    CHECK(items[0]->text() == QStringLiteral("EQ\tCtrl+E"));
    CHECK(items[1]->text() == QStringLiteral("Mute\tCtrl+M"));
    shortcuts.setGlobal(QStringLiteral("mute"), false);
    CHECK(items[1]->text() == QStringLiteral("Mute"));
    shortcuts.setGlobal(QStringLiteral("mute"), true);
    CHECK(items[2]->isSeparator());
    CHECK(items[3]->text() == QStringLiteral("Output"));
    CHECK(items[4]->text() == QStringLiteral("Preset"));
    CHECK(items[5]->isSeparator());
    CHECK(items[6]->text() == QStringLiteral("Open Isotone"));
    CHECK(items[7]->text() == QStringLiteral("Quit"));

    QAction* eq = items[0];
    CHECK(eq->isCheckable());
    CHECK(eq->isChecked() == session.eqOn());
    eq->trigger();
    CHECK_FALSE(session.eqOn());
    CHECK_FALSE(eq->isChecked());
    session.setEqOn(true);
    CHECK(eq->isChecked());

    QAction* mute = items[1];
    CHECK_FALSE(mute->isChecked());
    mute->trigger();
    CHECK(session.muted());
    CHECK(mute->isChecked());
    session.setMuted(false);
    CHECK_FALSE(mute->isChecked());

    // The shortcut text follows a rebinding.
    shortcuts.rebind(QStringLiteral("eq"), QStringLiteral("Ctrl+Shift+E"));
    CHECK(eq->text() == QStringLiteral("EQ\tCtrl+Shift+E"));
    shortcuts.replace(QStringLiteral("eq"), QStringLiteral("Ctrl+M"));
    CHECK(mute->text() == QStringLiteral("Mute"));

    // Output: every working output, the current one checked; picking one selects it.
    QMenu* output = items[3]->menu();
    REQUIRE(output != nullptr);
    CHECK(output->actions().size() == outputs.rowCount());
    for (int row = 0; row < outputs.rowCount(); ++row) {
        CAPTURE(row);
        QAction* a = output->actions()[row];
        CHECK(a->text() == outputs.data(outputs.index(row), Outputs::NameRole).toString());
        CHECK(a->isCheckable());
        CHECK(a->isChecked() == (row == outputs.currentRow()));
    }
    CHECK(items[3]->isEnabled() == (outputs.rowCount() > 0));
    if (outputs.rowCount() > 1) {
        const int other = outputs.currentRow() == 0 ? 1 : 0;
        output->actions()[other]->trigger();
        CHECK(outputs.currentRow() == other);
        CHECK(output->actions()[other]->isChecked());
    }

    // Preset: the names, the current one checked (none with the foundation's stub).
    QMenu* preset = items[4]->menu();
    REQUIRE(preset != nullptr);
    CHECK(preset->actions().size() == presets.names().size());
    CHECK(items[4]->isEnabled() == !presets.names().isEmpty());

    QSignalSpy open(&tray, &TrayMenu::openRequested);
    QSignalSpy quit(&tray, &TrayMenu::quitRequested);
    items[6]->trigger();
    items[7]->trigger();
    CHECK(open.count() == 1);
    CHECK(quit.count() == 1);

    CHECK(tray.tooltip() == QStringLiteral("Isotone\n%1 · %2").arg(outputs.currentName(), presets.currentName()));
}

TEST_CASE("the preview session is a fixed sample with a spectrum, and writes nowhere") {
    PreviewSession preview;
    REQUIRE(preview.rowCount() == 8);
    CHECK(preview.bandAt(0)->type == FilterType::LowShelf);
    CHECK(preview.bandAt(0)->fc == 105.0);
    CHECK(preview.bandAt(1)->gain_db == 5.1);
    CHECK(preview.selectedRow() == 3);
    CHECK(preview.outputChannels() == 2);
    const double freqs[] = {100.0, 1000.0, 10000.0};
    double levels[3];
    CHECK(preview.spectrumLevels(freqs, 3, levels));
    for (int i = 0; i < 3; ++i) {
        CHECK(levels[i] < 0.0);
        // On the plot: over its bottom, which is the preview's own top less the range.
        CHECK(levels[i] > preview.spectrumTopDb() - ResponseGraph::kSpectrumRangeDb);
    }
    CHECK(levels[2] < levels[0]);   // falling towards the treble, as music does
}

TEST_CASE("a second launch finds the running instance and asks for its window") {
#if defined(_WIN32)
    const QString name = SingleInstance::nameFor(QStringLiteral("C:/isotone-test-%1").arg(test_rig::pid()));
    // Windows paths are one path whatever their case and separators.
    CHECK(name == SingleInstance::nameFor(QStringLiteral("c:\\ISOTONE-TEST-%1").arg(test_rig::pid())));
    CHECK(name != SingleInstance::nameFor(QStringLiteral("C:/isotone-other-%1").arg(test_rig::pid())));
#else
    const QString name = SingleInstance::nameFor(QStringLiteral("/tmp/isotone-test-%1").arg(test_rig::pid()));
    CHECK(name != SingleInstance::nameFor(QStringLiteral("/tmp/ISOTONE-TEST-%1").arg(test_rig::pid())));
    CHECK(name != SingleInstance::nameFor(QStringLiteral("/tmp/isotone-other-%1").arg(test_rig::pid())));
#endif

    SingleInstance nobody(name);
    CHECK_FALSE(nobody.notifyRunning(true));

    SingleInstance first(name);
    REQUIRE(first.listen());
    QSignalSpy shown(&first, &SingleInstance::showRequested);
    // A later launch is another process: here another thread, so this one's event loop can answer.
    const auto launch = [&name](bool show) {
        std::atomic<int> result{-1};
        std::thread t([&] { result = SingleInstance(name).notifyRunning(show) ? 1 : 0; });
        while (result < 0) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        t.join();
        return result == 1;
    };
    CHECK(launch(true));
    CHECK(shown.count() == 1);

    // Started for the tray: nothing to show.
    CHECK(launch(false));
    CHECK(shown.count() == 1);
}

TEST_CASE("the preamp is taken out of the shown spectrum, and the scale follows the music") {
    // The owner, 2026-09-15: an EQ boost did nothing to the spectrum, and loud
    // passages were cut off at the top. Auto preamp sets the preamp to the opposite
    // of the boost, so what the output plays drops by as much as the boost lifts.
    double db[3] = {-30.0, -20.0, -40.0};
    EqSession::removePreamp(db, 3, -9.0);
    CHECK(db[0] == doctest::Approx(-21.0));
    CHECK(db[1] == doctest::Approx(-11.0));
    CHECK(db[2] == doctest::Approx(-31.0));
    EqSession::removePreamp(db, 3, 0.0);
    CHECK(db[0] == doctest::Approx(-21.0));   // no preamp, no change

    // The top of the scale: the loudest band plus a little headroom, up in a
    // moment and down slowly, and never outside its limits.
    const double quiet = EqSession::kSpectrumTopFloorDb;
    CHECK(EqSession::followTopDb(quiet, -20.0, 0.2) > quiet + 10.0);        // a second of music is not needed
    CHECK(EqSession::followTopDb(quiet, -20.0, 2.0) == doctest::Approx(-17.0).epsilon(0.01));
    const double loud = -17.0;
    CHECK(EqSession::followTopDb(loud, -60.0, 0.2) > loud - 5.0);           // it does not drop with a quiet moment
    CHECK(std::abs(EqSession::followTopDb(loud, -60.0, 0.016) - loud) < 0.3);   // one frame of quiet: barely
    // Clamped: a full-scale passage does not push the top over 0 dBFS, and silence
    // does not take it under the floor.
    CHECK(EqSession::followTopDb(-5.0, 20.0, 10.0) == doctest::Approx(EqSession::kSpectrumTopCeilingDb).epsilon(0.01));
    CHECK(EqSession::followTopDb(-40.0, -120.0, 60.0) == doctest::Approx(quiet).epsilon(0.01));
    // A step settles where the target is, not past it.
    double top = quiet;
    for (int i = 0; i < 600; ++i) top = EqSession::followTopDb(top, -24.0, 1.0 / 60.0);
    CHECK(top == doctest::Approx(-21.0).epsilon(0.01));
}

TEST_CASE("when the music stops the scale holds still and the curve falls off its bottom") {
    // The owner, 2026-09-16: the fade after the music stops ended in a flash. The
    // curve was hidden at a fixed -75 dBFS on the loudest FFT bin, which could be
    // anywhere on the plot, and the scale's top kept following the falling level,
    // so the plot's bottom sank with the curve and it never reached it.
    const double range = EqSession::kSpectrumRangeDb;
    CHECK(range == ResponseGraph::kSpectrumRangeDb);   // the graph draws the same scale

    // While music plays the top follows the loudest band, as before.
    EqSession::SpectrumScale scale{-45.0, false};
    for (int i = 0; i < 300; ++i) scale = EqSession::nextSpectrumScale(scale.top_db, true, -12.0, 1.0 / 60.0);
    CHECK(scale.top_db == doctest::Approx(-9.0).epsilon(0.01));
    CHECK(scale.drawn);

    // The music stops and the level falls, 1 dB a frame. The top does not move,
    // and the curve is drawn every frame until it is under the plot's bottom.
    const double top = scale.top_db;
    double level = -12.0;
    int frames_drawn = 0;
    while (true) {
        level -= 1.0;
        scale = EqSession::nextSpectrumScale(scale.top_db, false, level, 1.0 / 60.0);
        CHECK(scale.top_db == top);
        if (!scale.drawn) break;
        ++frames_drawn;
        REQUIRE(frames_drawn < 1000);
    }
    // It went away at the bottom, not before: the last level drawn was on the plot
    // and the first one hidden is under it.
    CHECK(level < top - range);
    CHECK(level + 1.0 >= top - range);
    CHECK(frames_drawn == static_cast<int>(std::floor((-12.0 - (top - range)))));

    // Music again: the top follows once more.
    scale = EqSession::nextSpectrumScale(scale.top_db, true, -30.0, 2.0);
    CHECK(scale.top_db < top - 10.0);
    CHECK(scale.drawn);

    // Nothing has played at all: nothing drawn, whatever the analyzer's floor says.
    CHECK_FALSE(EqSession::nextSpectrumScale(EqSession::kSpectrumTopFloorDb, false, -120.0, 1.0 / 60.0).drawn);
}

TEST_CASE("the spectrum decay reaches the session's analyzer") {
    EqSession session;
    // Pinned at the highest resolution; only the decay is a setting (owner, 2026-09-15).
    CHECK(session.spectrumAnalyzer().fft_size() == isotone::ui::SpectrumAnalyzer::kFftSize);
    session.setSpectrumDecayMs(120.0);
    CHECK(session.spectrumAnalyzer().release_ms() == 120.0);
    session.setSpectrumDecayMs(0.0);   // not a decay: kept
    CHECK(session.spectrumAnalyzer().release_ms() == 120.0);
}
