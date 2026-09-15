// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Settings with Qt, without QML: the graph's ranges, the shortcut registry,
// global hotkeys, the tray menu and the Appearance preview.

#include "doctest.h"

#include <QAction>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <windows.h>

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
#include "traymenu.h"

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

QAction* find_action(QMenu* menu, const QString& prefix) {
    for (QAction* a : menu->actions())
        if (a->text().startsWith(prefix)) return a;
    return nullptr;
}

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
    // The default range draws what the boards show.
    auto lines = ResponseGraph::gridLines(20, 20000);
    std::vector<double> hz;
    for (const auto& l : lines) hz.push_back(l.hz);
    CHECK(hz == std::vector<double>{20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800,
                                    1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000});
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
        // Off until turned on: a global Ctrl+Left takes word navigation from every other app.
        CHECK_FALSE(r.isGlobal(QString::fromLatin1(id)));
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

TEST_CASE("the tray menu") {
    Scratch s;
    ShortcutRegistry shortcuts(s.settings.get());
    EqSession session;
    Outputs outputs;
    // No output list and a Local\ region namespace: nothing reaches a real output.
    Presets presets(s.dir.path(), &session, {}, L"Local\\isotone-tray-test.", L"");
    TrayMenu tray(&session, &outputs, &presets, &shortcuts);
    QMenu* menu = tray.menu();

    // EQ, Mute, -, Output, Preset, -, Open Isotone, Quit
    const QList<QAction*> items = menu->actions();
    REQUIRE(items.size() == 8);
    CHECK(items[0]->text() == QStringLiteral("EQ\tCtrl+E"));
    CHECK(items[1]->text() == QStringLiteral("Mute\tCtrl+M"));
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
    double levels[3], peaks[3];
    CHECK(preview.spectrumLevels(freqs, 3, levels));
    CHECK(preview.spectrumPeakLevels(freqs, 3, peaks));
    for (int i = 0; i < 3; ++i) {
        CHECK(levels[i] < 0.0);
        CHECK(levels[i] > -90.0);
        CHECK(peaks[i] >= levels[i]);
    }
    CHECK(levels[2] < levels[0]);   // falling towards the treble, as music does
}

TEST_CASE("a second launch finds the running instance and asks for its window") {
    const QString name = SingleInstance::nameFor(QStringLiteral("C:/isotone-test-%1").arg(GetCurrentProcessId()));
    CHECK(name == SingleInstance::nameFor(QStringLiteral("c:\\ISOTONE-TEST-%1").arg(GetCurrentProcessId())));
    CHECK(name != SingleInstance::nameFor(QStringLiteral("C:/isotone-other-%1").arg(GetCurrentProcessId())));

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

TEST_CASE("the spectrum options reach the session's analyzer") {
    EqSession session;
    session.setSpectrumOptions(16384, 120.0, 4.5);
    CHECK(session.spectrumAnalyzer().fft_size() == 16384);
    session.setSpectrumOptions(1000, 120.0, 4.5);   // not a resolution: kept
    CHECK(session.spectrumAnalyzer().fft_size() == 16384);
    session.setSpectrumOptions(4096, 120.0, 0.0);
    CHECK(session.spectrumAnalyzer().fft_size() == 4096);
}
