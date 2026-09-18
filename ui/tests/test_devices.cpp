// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Devices: the status mapping from devicetool's status JSON, the actions and
// plans each status gives, and DevicetoolController's phases through a scripted
// runner and through a real `serve` launched unelevated.
//
// tests/devices/status-*.json are `isotone-devicetool status` captured on this
// machine on 2026-09-15 (installed: CABLE Input; not_installed: Anker USB Audio;
// eapo: Steam Streaming Speakers; unplugged_eapo: AirPods). The states this
// machine has no endpoint in are status-derived-*.json, made from those captures
// by changing only the fields each state's decision reads (the script is
// described in docs/notes/stage4-devices.md).
//
// Nothing here changes the machine: changing commands go only to the scripted
// runner, or to an unelevated serve, which refuses them with exit code 3, after
// checking this process is not elevated.

#include "doctest.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <mmdeviceapi.h>
#include <objbase.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "apppaths.h"
#include "appsettings.h"
#include "devicesmodel.h"
#include "devicestatus.h"
#include "devicetoolcontroller.h"
#include "devicetoolrunner.h"
#include "eapo_install.h"
#include "equalizerapoconfig.h"
#include "outputs.h"

namespace fs = std::filesystem;

namespace {

QByteArray sample(const char* name) {
    QFile f(QStringLiteral(ISOTONE_UI_TEST_DATA "/status-%1.json").arg(QLatin1String(name)));
    REQUIRE_MESSAGE(f.open(QIODevice::ReadOnly), name);
    return f.readAll();
}

DeviceFacts facts(const char* name) {
    DeviceFacts f;
    REQUIRE_MESSAGE(factsFromStatusJson(sample(name), &f), name);
    return f;
}

std::string text(const QString& s) { return s.toStdString(); }
std::string text(const QStringList& l) { return l.join(QLatin1Char('|')).toStdString(); }

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
    CloseHandle(token);
    return !ok || e.TokenIsElevated != 0;
}

const QString kCable = QStringLiteral("{798436d2-8c71-4834-9248-00ccbaaca00a}");
const std::wstring kCableW = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";

ScriptedRunner::Answer ok(const std::string& json = "{\"ok\":true}") { return {ERROR_SUCCESS, 0, json, 0}; }

// The controller's phases as they change, and waiting for it to settle.
struct Watch {
    DevicetoolController& c;
    QStringList phases;
    explicit Watch(DevicetoolController& controller) : c(controller) {
        QObject::connect(&c, &DevicetoolController::changed, [this] {
            if (phases.isEmpty() || phases.back() != c.phase()) phases.push_back(c.phase());
        });
    }
    void settle() {
        QElapsedTimer t;
        t.start();
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        } while ((c.working() || c.phase().isEmpty()) && t.elapsed() < 20000);
        // The worker may still be publishing the last change.
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        REQUIRE_FALSE(c.working());
    }
};

std::string calls(const ScriptedRunner& r) {
    std::string out;
    for (const ScriptedRunner::Call& call : r.calls()) {
        if (!out.empty()) out += " ; ";
        out += call.direct ? "direct:" : "";
        for (size_t i = 0; i < call.args.size(); ++i)
            out += (i ? " " : "") + QString::fromStdWString(call.args[i]).toStdString();
    }
    return out;
}

struct Controller {
    ScriptedRunner* runner;
    DevicetoolController c;
    Watch watch;
    explicit Controller(std::unique_ptr<ScriptedRunner> r = std::make_unique<ScriptedRunner>())
        : runner(r.get()), c(std::move(r)), watch(c) {}
};

}  // namespace

TEST_CASE("every status maps from devicetool's status JSON") {
    struct Case {
        const char* file;
        const char* key, *label, *dot, *column, *detail, *now, *actions, *effect_slots;
    };
    const Case cases[] = {
        {"installed", "installed", "Installed", "ok", "Native", "Native (IsoAPO)", "IsoAPO", "test|uninstall", "MFX"},
        {"eapo", "active", "Active", "ok", "Equalizer APO", "Equalizer APO", "Equalizer APO", "replace|test", "LFX + GFX"},
        {"not_installed", "not_installed", "Not installed", "off", "\xE2\x80\x94", "\xE2\x80\x94", "Off", "install", ""},
        {"unplugged_eapo", "unplugged", "Unplugged", "off", "Equalizer APO", "\xE2\x80\x94", "Equalizer APO", "", "SFX + EFX"},
        {"derived-disabled", "unplugged", "Unplugged", "off", "\xE2\x80\x94", "\xE2\x80\x94", "Off", "", ""},
        {"derived-detached", "detached", "Detached", "warn", "Native", "Native (IsoAPO)", "IsoAPO", "repair|test|uninstall", "MFX"},
        {"derived-detached-no-mode", "detached", "Detached", "warn", "Native", "Native (IsoAPO)", "IsoAPO", "repair|test|uninstall", ""},
        {"derived-detached-equalizerapo", "detached", "Detached", "warn", "Equalizer APO", "Equalizer APO", "Equalizer APO",
         "takeBack|keepEapo", "LFX + GFX"},
        {"derived-replaced", "replaced", "Replaced by Equalizer APO", "warn", "Equalizer APO", "Equalizer APO", "Equalizer APO",
         "takeBack|keepEapo", "LFX + GFX"},
        {"derived-alongside", "conflict", "Conflict", "bad", "Native + Equalizer APO", "IsoAPO + Equalizer APO",
         "IsoAPO + Equalizer APO", "removeEapo|uninstall", "MFX + SFX"},
        {"derived-interrupted", "interrupted", "Interrupted", "warn", "Native", "Native (IsoAPO)", "IsoAPO", "undo", "MFX"},
        {"derived-unrecorded", "unrecorded", "Unrecorded", "warn", "Native", "Native (IsoAPO)", "IsoAPO", "copyDiagnostics", "MFX"},
        {"derived-enhancements-off", "enhancements_off", "Enhancements off", "warn", "Native", "Native (IsoAPO)", "IsoAPO",
         "enableEnhancements", "MFX"},
    };
    for (const Case& k : cases) {
        CAPTURE(k.file);
        const DeviceFacts f = facts(k.file);
        CHECK(text(statusKey(f)) == k.key);
        CHECK(text(statusLabel(statusKey(f))) == k.label);
        CHECK(text(statusDot(statusKey(f))) == k.dot);
        CHECK(text(engineColumn(f)) == k.column);
        CHECK(text(engineDetail(f)) == k.detail);
        CHECK(text(nowEngine(f)) == k.now);
        CHECK(text(actions(f)) == k.actions);
        CHECK(text(f.effect_slots) == k.effect_slots);
        CHECK(working(f) == (std::string(k.key) == "installed" || std::string(k.key) == "active"));
    }
}

TEST_CASE("status JSON gives the name, format, default and remedies") {
    DeviceFacts f = facts("installed");
    CHECK(f.guid == kCable);
    CHECK(text(f.name) == "CABLE Input (VB-Audio Virtual Cable)");
    CHECK(f.present);
    CHECK_FALSE(f.default_console);
    CHECK(text(formatLabel(f)) == "48 kHz · 2 ch");
    CHECK(text(f.install_mode) == "SFX_MFX");

    f = facts("derived-installed-default-8ch-96k");
    CHECK(f.default_console);
    CHECK(text(formatLabel(f)) == "96 kHz · 8 ch");

    f = facts("unplugged_eapo");
    CHECK_FALSE(f.present);
    CHECK(text(formatLabel(f)) == "44.1 kHz · 2 ch");

    f = facts("derived-detached-no-mode");
    CHECK(text(f.remedies) == "repair --mode|uninstall");
    CHECK(text(f.default_mode) == "SFX_MFX");

    DeviceFacts none;
    CHECK_FALSE(factsFromStatusJson("{\"command\":\"list\",\"ok\":true}", &none));
    CHECK_FALSE(factsFromStatusJson("not json", &none));
}

TEST_CASE("each action runs devicetool's remedy for the state") {
    const auto op = [](const char* file, const char* action) {
        const QVariantMap m = operation(facts(file), QString::fromLatin1(action));
        return m.value(QStringLiteral("kind")).toString().toStdString() + ": " +
               m.value(QStringLiteral("args")).toStringList().join(QLatin1Char(' ')).toStdString();
    };
    const std::string cable = kCable.toStdString();
    CHECK(op("not_installed", "install") == "install: install {407cef09-cb03-4063-a26f-2ff82a1c0e4a}");
    CHECK(op("installed", "uninstall") == "uninstall: uninstall " + cable);
    CHECK(op("installed", "test") == "test: test " + cable);
    // Repair names the output: another endpoint's refusal or mode must not decide it.
    CHECK(op("derived-detached", "repair") == "repair: repair " + cable);
    // The record predates the install mode: repair takes the mode an install would pick.
    CHECK(op("derived-detached-no-mode", "repair") == "repair: repair " + cable + " --mode mfx");
    CHECK(op("derived-alongside", "removeEapo") == "replace: install " + cable + " --replace-equalizerapo");
    CHECK(op("derived-replaced", "takeBack") == "replace: install {6cd5cc5c-be4c-4f7a-8090-20f6cb934c21} --replace-equalizerapo");
    CHECK(op("derived-replaced", "keepEapo") == "uninstall: uninstall {6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}");
    CHECK(op("derived-interrupted", "undo") == "repair: repair " + cable);
    CHECK(op("derived-enhancements-off", "enableEnhancements") == "repair: enable-enhancements " + cable);
    // After the Replace dialog's choice of IsoAPO.
    CHECK(op("eapo", "replaceWithIsoApo") == "replace: install {6cd5cc5c-be4c-4f7a-8090-20f6cb934c21} --replace-equalizerapo");
    // A dialog, not a command.
    CHECK(operation(facts("eapo"), QStringLiteral("replace")).isEmpty());
    CHECK(operation(facts("derived-unrecorded"), QStringLiteral("copyDiagnostics")).isEmpty());
}

TEST_CASE("Settings Outputs plans each engine choice") {
    const auto plan = [](const char* file, const char* want) {
        const QVariantMap m = planChange(facts(file), QString::fromLatin1(want));
        std::string out;
        for (const QVariant& c : m.value(QStringLiteral("commands")).toList())
            out += c.toStringList().join(QLatin1Char(' ')).toStdString() + "; ";
        if (m.value(QStringLiteral("attach")).toBool()) out += "attach; ";
        if (m.value(QStringLiteral("removeBlock")).toBool()) out += "remove block; ";
        return out + "-> " + m.value(QStringLiteral("result")).toString().toStdString() +
               (m.value(QStringLiteral("changed")).toBool() ? "" : " (unchanged)");
    };
    const std::string cable = kCable.toStdString();
    const std::string steam = "{6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}";
    CHECK(plan("installed", "IsoAPO") == "-> installed (unchanged)");
    CHECK(plan("installed", "Off") == "uninstall " + cable + "; -> removed");
    CHECK(plan("not_installed", "IsoAPO") == "install {407cef09-cb03-4063-a26f-2ff82a1c0e4a}; -> installed");
    CHECK(plan("not_installed", "Off") == "-> removed (unchanged)");
    CHECK(plan("eapo", "IsoAPO") == "install " + steam + " --replace-equalizerapo; -> installed");
    CHECK(plan("eapo", "Equalizer APO") == "-> attached (unchanged)");
    CHECK(plan("eapo", "Off") == "remove block; -> removed");
    CHECK(plan("derived-alongside", "IsoAPO") == "install " + cable + " --replace-equalizerapo; -> installed");
    CHECK(plan("derived-alongside", "Equalizer APO") == "uninstall " + cable + "; attach; -> attached");
    CHECK(plan("derived-alongside", "Off") == "uninstall " + cable + "; remove block; -> removed");
    CHECK(plan("derived-replaced", "IsoAPO") == "install " + steam + " --replace-equalizerapo; -> installed");
    CHECK(plan("derived-replaced", "Off") == "uninstall " + steam + "; remove block; -> removed");
    CHECK(plan("derived-detached", "Off") == "uninstall " + cable + "; -> removed");
    CHECK(planChange(facts("installed"), QStringLiteral("IsoAPO")).value(QStringLiteral("guid")).toString() == kCable);
    // Off is kept only where Equalizer APO stays on the output.
    const auto off = [](const char* file, const char* want) {
        return planChange(facts(file), QString::fromLatin1(want)).value(QStringLiteral("off")).toBool();
    };
    CHECK(off("eapo", "Off"));
    CHECK(off("derived-alongside", "Off"));
    CHECK_FALSE(off("installed", "Off"));
    CHECK_FALSE(off("eapo", "IsoAPO"));
}

// ---------------------------------------------------------------------------
// Review fixes: shared helpers

namespace {

// A sandbox Equalizer APO config directory, and settings in a directory of their own.
struct ConfigBox {
    fs::path dir;
    QTemporaryDir data;
    QString previous_compat, previous_data;
    explicit ConfigBox(const std::string& config) {
        dir = fs::temp_directory_path() /
              ("isotone-review-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(dir);
        REQUIRE_FALSE(isotone::compat::is_live_install_path(dir));
        write("config.txt", config);
        previous_compat = AppPaths::compatConfigDir();
        previous_data = AppPaths::dataDir();
        AppPaths::setCompatConfigDir(QString::fromStdWString(dir.wstring()));
        AppPaths::setDataDir(data.path());
    }
    ~ConfigBox() {
        AppPaths::setCompatConfigDir(previous_compat);
        AppPaths::setDataDir(previous_data);
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    void write(const char* name, const std::string& bytes) const { std::ofstream(dir / name, std::ios::binary) << bytes; }
    std::string read(const char* name) const {
        std::stringstream s;
        s << std::ifstream(dir / name, std::ios::binary).rdbuf();
        return s.str();
    }
};

const QString kSteam = QStringLiteral("{6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}");
const char kSteamBlock[] = "Device: {6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}\r\nChannel: all\r\nPreamp: -3 dB\r\n";

// Forwards to a runner the test keeps, so its calls can be read after the controller is gone.
struct Forward : DevicetoolRunner {
    std::shared_ptr<ScriptedRunner> r;
    explicit Forward(std::shared_ptr<ScriptedRunner> runner) : r(std::move(runner)) {}
    DWORD start() override { return r->start(); }
    bool started() const override { return r->started(); }
    DevicetoolResult run(const std::vector<std::wstring>& args) override { return r->run(args); }
    DevicetoolResult runDirect(const std::vector<std::wstring>& args) override { return r->runDirect(args); }
    void cancel() override { r->cancel(); }
};

}  // namespace

TEST_CASE("an Equalizer APO output whose config.txt does not include Isotone.txt is Not attached, with Attach") {
    DeviceFacts f = facts("eapo");
    f.attached = false;
    CHECK(text(statusKey(f)) == "not_attached");
    CHECK(text(statusLabel(statusKey(f))) == "Not attached");
    CHECK(text(statusDot(statusKey(f))) == "warn");
    CHECK(text(actions(f)) == "attach|test");
    CHECK_FALSE(working(f));
    CHECK(operation(f, QStringLiteral("attach")).isEmpty());   // the Attach dialog
    // Only Equalizer APO outputs.
    DeviceFacts native = facts("installed");
    native.attached = false;
    CHECK(text(statusKey(native)) == "installed");
}

TEST_CASE("Devices reads config.txt's include and Off, and says when what Outputs lists changed") {
    ConfigBox box("Include: peace.txt\r\n");
    QFile status(QStringLiteral(ISOTONE_UI_TEST_DATA "/status-eapo.json"));
    REQUIRE(status.open(QIODevice::ReadOnly));
    box.write("script.json", "{\"devices\":[" + status.readAll().toStdString() + "]}");
    // Scripted from the start: no read of the machine on another thread.
    const QByteArray previous = qgetenv("ISOTONE_FAKE_DEVICETOOL");
    qputenv("ISOTONE_FAKE_DEVICETOOL", QString::fromStdWString((box.dir / "script.json").wstring()).toUtf8());
    DevicesModel model;
    qunsetenv("ISOTONE_FAKE_DEVICETOOL");
    if (!previous.isEmpty()) qputenv("ISOTONE_FAKE_DEVICETOOL", previous);
    QSignalSpy outputs(&model, &DevicesModel::outputsChanged);
    CHECK(text(model.row(kSteam).value(QStringLiteral("status")).toString()) == "not_attached");

    model.refresh();
    CHECK(outputs.count() == 0);   // nothing changed

    box.write("config.txt", "Include: peace.txt\r\nInclude: Isotone.txt\r\n");
    model.refresh();
    CHECK(text(model.row(kSteam).value(QStringLiteral("status")).toString()) == "active");
    CHECK(outputs.count() == 1);

    setEqualizerApoOutputOff(kSteam, true);
    model.refresh();
    CHECK(text(model.row(kSteam).value(QStringLiteral("now")).toString()) == "Off");
    CHECK(outputs.count() == 2);
}

TEST_CASE("Outputs lists an Equalizer APO output only while config.txt includes Isotone.txt and it is not Off") {
    ConfigBox box("Include: peace.txt\r\n");
    // COM stays initialized for Outputs; uninitialized only if this call did it.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Uninit {
        HRESULT hr;
        ~Uninit() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } uninit{com};
    std::vector<isotone::devices::Endpoint> endpoints;
    REQUIRE(SUCCEEDED(isotone::devices::enumerate_render_endpoints(&endpoints)));
    QStringList eapo;
    for (const isotone::devices::Endpoint& e : endpoints)
        if (e.state == DEVICE_STATE_ACTIVE && e.format.present && e.engine.backend == isotone::devices::Backend::equalizerapo)
            eapo << QString::fromStdWString(e.guid);
    if (eapo.isEmpty()) {
        MESSAGE("SKIPPED: no active output with Equalizer APO here");
        return;
    }
    const auto listed = [](const Outputs& o, const QString& guid) {
        for (const Outputs::Output& out : o.outputs())
            if (QString::fromStdString(out.guid) == guid) return true;
        return false;
    };
    Outputs outputs;
    for (const QString& g : eapo) CHECK_MESSAGE(!listed(outputs, g), g.toStdString());

    box.write("config.txt", "Include: peace.txt\r\nInclude: Isotone.txt\r\n");
    outputs.refresh();
    for (const QString& g : eapo) CHECK_MESSAGE(listed(outputs, g), g.toStdString());

    setEqualizerApoOutputOff(eapo.first(), true);
    outputs.refresh();
    CHECK_FALSE(listed(outputs, eapo.first()));
    for (qsizetype i = 1; i < eapo.size(); ++i) CHECK(listed(outputs, eapo[i]));
    MESSAGE(eapo.size() << " Equalizer APO outputs checked");
}

TEST_CASE("Off on an Equalizer APO output is kept before its block goes, and Equalizer APO undoes it") {
    ConfigBox box("Include: Isotone.txt\r\n");
    box.write("Isotone.txt", kSteamBlock);
    // AppSettings reads what the controller keeps.
    AppSettings settings;

    Controller k;
    k.runner->setStarted(true);
    bool off_when_signalled = false;
    QObject::connect(&k.c, &DevicetoolController::outputChoicesChanged,
                     [&] { off_when_signalled = equalizerApoOutputOff(kSteam); });
    QSignalSpy choices(&k.c, &DevicetoolController::outputChoicesChanged);
    k.c.apply({planChange(facts("eapo"), QStringLiteral("Off"))});
    // At once, before the worker removes the block.
    CHECK(choices.count() == 1);
    CHECK(off_when_signalled);
    k.watch.settle();
    CHECK(text(k.c.rowStatus().value(kSteam).toString()) == "removed");
    CHECK(box.read("Isotone.txt").find("6cd5cc5c") == std::string::npos);
    CHECK(equalizerApoOutputOff(kSteam));
    CHECK(settings.value(QStringLiteral("outputs/off/") + kSteam).toBool());
    CHECK_FALSE(equalizerApoOutputListed(kSteam, true));

    DeviceFacts now = facts("eapo");
    now.isotone_off = true;
    CHECK(text(nowEngine(now)) == "Off");
    const QVariantMap back = planChange(now, QStringLiteral("Equalizer APO"));
    CHECK(back.value(QStringLiteral("commands")).toList().isEmpty());   // no IsoAPO to uninstall
    CHECK(back.value(QStringLiteral("attach")).toBool());
    CHECK(back.value(QStringLiteral("changed")).toBool());
    k.c.apply({back});
    k.watch.settle();
    CHECK_FALSE(equalizerApoOutputOff(kSteam));
    CHECK(equalizerApoOutputListed(kSteam, true));
    CHECK(choices.count() == 2);
}

TEST_CASE("Off goes back when its apply is declined") {
    ConfigBox box("Include: Isotone.txt\r\n");
    box.write("Isotone.txt", kSteamBlock);
    Controller k;
    k.runner->setStart(ERROR_CANCELLED);
    QSignalSpy choices(&k.c, &DevicetoolController::outputChoicesChanged);
    // derived-alongside: IsoAPO to uninstall, so approval is asked.
    k.c.apply({planChange(facts("derived-alongside"), QStringLiteral("Off"))});
    CHECK(equalizerApoOutputOff(kCable));
    k.watch.settle();
    CHECK(text(k.c.phase()) == "declined");
    CHECK_FALSE(equalizerApoOutputOff(kCable));
    CHECK(choices.count() == 2);
}

TEST_CASE("a repair that fails after changing the output restarts audio all the same") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("repair", {ERROR_SUCCESS, 1,
                                "{\"command\":\"repair\",\"ok\":false,\"fx_properties_changed\":true,\"repaired\":[{\"guid\":\"{798436d2-8c71-4834-9248-00ccbaaca00a}\",\"ok\":false,\"undid_interrupted\":\"install\",\"error\":\"Equalizer APO is on this endpoint again\"}]}",
                                0});
    k.c.run(QStringLiteral("repair"), kCable, {QStringLiteral("repair"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|restarting|failed");
    CHECK(calls(*k.runner) == "repair " + kCable.toStdString() + " ; restart-audio");
    CHECK(text(k.c.reason()) == "Equalizer APO is on this endpoint again.");
    CHECK(k.c.details().contains(QStringLiteral("isotone-devicetool repair")));   // Copy details: the repair's
}

TEST_CASE("Retry after a failed test runs the test again, not the change") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("test", {ERROR_SUCCESS, 1, "{\"command\":\"test\",\"ok\":false,\"reason\":\"audio client initialization failed\"}", 0});
    k.runner->answer("test", ok());
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    REQUIRE(text(k.c.phase()) == "failed");
    CHECK(text(k.c.kind()) == "test");
    k.watch.phases.clear();
    k.c.retry();
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|done");
    CHECK(text(k.c.kind()) == "test");
    const std::string cable = kCable.toStdString();
    CHECK(calls(*k.runner) == "install " + cable + " ; restart-audio ; direct:test " + cable + " ; direct:test " + cable);
}

TEST_CASE("restart-audio answering busy is busy, and Retry restarts and tests without changing again") {
    const std::string cable = kCable.toStdString();
    const ScriptedRunner::Answer busy{ERROR_SUCCESS, 4, "{\"command\":\"restart-audio\",\"ok\":false,\"error\":\"busy\",\"reason\":\"another devicetool run held the machine lock for 60000 ms\"}", 0};

    SUBCASE("an operation") {
        Controller k;
        k.runner->setStarted(true);
        k.runner->answer("restart-audio", busy);
        k.runner->answer("restart-audio", ok());
        k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
        k.watch.settle();
        CHECK(text(k.watch.phases) == "running|restarting|busy");
        CHECK(calls(*k.runner) == "install " + cable + " ; restart-audio");
        k.watch.phases.clear();
        k.c.retry();
        k.watch.settle();
        CHECK(text(k.watch.phases) == "running|restarting|testing|done");
        CHECK(text(k.c.kind()) == "install");
        CHECK(calls(*k.runner) == "install " + cable + " ; restart-audio ; restart-audio ; direct:test " + cable);
    }
    SUBCASE("an apply, whose rows stay") {
        Controller k;
        k.runner->setStarted(true);
        k.runner->answer("restart-audio", busy);
        k.runner->answer("restart-audio", ok());
        const std::string anker = "{407cef09-cb03-4063-a26f-2ff82a1c0e4a}";
        k.c.apply({planChange(facts("not_installed"), QStringLiteral("IsoAPO"))});
        k.watch.settle();
        CHECK(text(k.c.phase()) == "busy");
        CHECK(text(k.c.rowStatus().value(QString::fromStdString(anker)).toString()) == "installed");
        k.c.retry();
        k.watch.settle();
        CHECK(text(k.c.phase()) == "done");
        CHECK(text(k.c.rowStatus().value(QString::fromStdString(anker)).toString()) == "installed");
        CHECK(calls(*k.runner) == "install " + anker + " ; restart-audio ; restart-audio ; direct:test " + anker);
    }
}

TEST_CASE("closing while applying stops between plans, and restarts and tests nothing") {
    auto runner = std::make_shared<ScriptedRunner>();
    runner->setStarted(true);
    runner->answer("install", {ERROR_SUCCESS, 0, "{\"ok\":true}", -1});   // held until cancel
    {
        DevicetoolController c(std::make_unique<Forward>(runner));
        c.apply({planChange(facts("not_installed"), QStringLiteral("IsoAPO")), planChange(facts("installed"), QStringLiteral("Off"))});
        QElapsedTimer t;
        t.start();
        while (runner->calls().empty() && t.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        REQUIRE(runner->calls().size() == 1);
    }   // the destructor, as closing the app runs it
    CHECK(calls(*runner) == "install {407cef09-cb03-4063-a26f-2ff82a1c0e4a}");
}

TEST_CASE("the config steps refuse an empty config directory and touch nothing") {
    ConfigBox box("Include: peace.txt\r\n");
    box.write("Isotone.txt", kSteamBlock);
    // Relative to the working directory is where an empty path would land.
    const fs::path cwd = fs::current_path();
    fs::current_path(box.dir);
    const QString attach = attachConfigStep({});
    const QString remove = removeBlockStep({}, kSteam);
    fs::current_path(cwd);
    CHECK(text(attach) == text(windowsMessage(ERROR_PATH_NOT_FOUND)));
    CHECK(text(remove) == text(windowsMessage(ERROR_PATH_NOT_FOUND)));
    CHECK(box.read("config.txt") == "Include: peace.txt\r\n");
    CHECK(box.read("Isotone.txt") == kSteamBlock);
}

TEST_CASE("a fake devicetool refuses to run without a sandbox config directory, and follows the environment") {
    const QByteArray previous = qgetenv("ISOTONE_FAKE_DEVICETOOL");
    const bool was_set = qEnvironmentVariableIsSet("ISOTONE_FAKE_DEVICETOOL");
    const QString compat = AppPaths::compatConfigDir();

    qunsetenv("ISOTONE_FAKE_DEVICETOOL");
    AppPaths::setCompatConfigDir(QString());
    CHECK_FALSE(fakeDevicetool());
    CHECK(fakeDevicetoolRefusal().isEmpty());

    qputenv("ISOTONE_FAKE_DEVICETOOL", "script.json");
    CHECK(fakeDevicetool());
    CHECK_FALSE(fakeDevicetoolRefusal().isEmpty());
    AppPaths::setCompatConfigDir(QStringLiteral("C:\\sandbox"));
    CHECK(fakeDevicetoolRefusal().isEmpty());

    AppPaths::setCompatConfigDir(compat);
    if (was_set)
        qputenv("ISOTONE_FAKE_DEVICETOOL", previous);
    else
        qunsetenv("ISOTONE_FAKE_DEVICETOOL");
}

TEST_CASE("an endpoint read in process maps as devicetool status maps it, on every present output here") {
    const std::wstring exe = devicetoolPath().toStdWString();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MESSAGE("SKIPPED: no isotone-devicetool at " << devicetoolPath().toStdString());
        return;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<isotone::devices::Endpoint> endpoints;
    REQUIRE(SUCCEEDED(isotone::devices::enumerate_render_endpoints(&endpoints)));
    size_t compared = 0;
    for (const isotone::devices::Endpoint& e : endpoints) {
        if (e.state & DEVICE_STATE_NOTPRESENT) continue;
        CAPTURE(QString::fromStdWString(e.guid).toStdString());
        const DeviceFacts in_process = factsFromEndpoint(e);
        const DevicetoolResult r = runDevicetoolDirect(exe, {L"status", e.guid});
        REQUIRE(r.error == ERROR_SUCCESS);
        REQUIRE(r.exit_code == 0);
        DeviceFacts from_json;
        REQUIRE(factsFromStatusJson(QByteArray::fromStdString(r.json), &from_json));
        CHECK(text(statusKey(in_process)) == text(statusKey(from_json)));
        CHECK(text(engineColumn(in_process)) == text(engineColumn(from_json)));
        CHECK(text(formatLabel(in_process)) == text(formatLabel(from_json)));
        CHECK(in_process.guid == from_json.guid);
        CHECK(in_process.present == from_json.present);
        ++compared;
    }
    CoUninitialize();
    MESSAGE(compared << " present render endpoints compared");
}

// ---------------------------------------------------------------------------
// The controller through a scripted runner

TEST_CASE("an install asks for approval, installs, restarts audio and tests the output") {
    Controller k;
    QSignalSpy finished(&k.c, &DevicetoolController::finished);
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "uac|running|restarting|testing|done");
    CHECK(calls(*k.runner) == "install " + kCable.toStdString() + " ; restart-audio ; direct:test " + kCable.toStdString());
    CHECK(k.c.elevated());
    CHECK(text(k.c.kind()) == "install");
    CHECK(k.c.target() == kCable);
    CHECK(finished.count() == 1);

    SUBCASE("the next change asks nothing more") {
        k.watch.phases.clear();
        k.c.run(QStringLiteral("uninstall"), kCable, {QStringLiteral("uninstall"), kCable});
        k.watch.settle();
        CHECK(text(k.watch.phases) == "running|restarting|testing|done");
    }
}

TEST_CASE("a failed command shows devicetool's reason as a sentence, and restarts nothing") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("install", {ERROR_SUCCESS, 1,
                                 "{\"command\":\"install\",\"ok\":false,\"reason\":\"an install record exists but IsoAPO is in no slot (detached); run repair\"}",
                                 0});
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|failed");
    CHECK(text(k.c.reason()) == "An install record exists but IsoAPO is in no slot (detached); run repair.");
    CHECK(calls(*k.runner) == "install " + kCable.toStdString());
    CHECK(k.c.details().contains(QStringLiteral("exit 1")));
    CHECK(k.c.details().contains(QStringLiteral("an install record exists")));

    SUBCASE("Retry runs it again") {
        k.runner->answer("install", ok());
        k.watch.phases.clear();
        k.c.retry();
        k.watch.settle();
        CHECK(text(k.watch.phases) == "running|restarting|testing|done");
    }
}

TEST_CASE("repair's reason comes from the endpoint that failed, and each repaired output is tested") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("repair", {ERROR_SUCCESS, 1,
                                "{\"command\":\"repair\",\"ok\":false,\"repaired\":[{\"guid\":\"{a}\",\"ok\":false,\"error\":\"Equalizer APO is on this endpoint again; run install --replace-equalizerapo to replace it, or uninstall to keep Equalizer APO\"}]}",
                                0});
    k.c.run(QStringLiteral("repair"), kCable, {QStringLiteral("repair")});
    k.watch.settle();
    CHECK(text(k.c.phase()) == "failed");
    CHECK(text(k.c.reason()) ==
          "Equalizer APO is on this endpoint again; run install --replace-equalizerapo to replace it, or uninstall to keep Equalizer APO.");

    k.runner->answer("repair", ok("{\"command\":\"repair\",\"ok\":true,\"repaired\":[{\"guid\":\"{b}\",\"ok\":true,\"mode\":\"SFX_MFX\"},{\"guid\":\"{798436d2-8c71-4834-9248-00ccbaaca00a}\",\"ok\":true,\"mode\":\"SFX_MFX\"}]}"));
    k.watch.phases.clear();
    k.c.retry();
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|restarting|testing|done");
    CHECK(calls(*k.runner) == "repair ; repair ; restart-audio ; direct:test " + kCable.toStdString() + " ; direct:test {b}");
}

TEST_CASE("exit code 4 is busy, and Retry runs it again") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("repair", {ERROR_SUCCESS, 4, "{\"command\":\"repair\",\"ok\":false,\"error\":\"busy\",\"reason\":\"another devicetool run held the machine lock for 60000 ms\"}", 0});
    k.runner->answer("repair", ok());
    k.c.run(QStringLiteral("repair"), kCable, {QStringLiteral("repair")});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|busy");
    k.c.retry();
    k.watch.settle();
    CHECK(text(k.c.phase()) == "done");
}

TEST_CASE("declined approval runs nothing, and Retry asks again") {
    Controller k;
    k.runner->setStart(ERROR_CANCELLED);
    k.c.run(QStringLiteral("repair"), kCable, {QStringLiteral("repair")});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "uac|declined");
    CHECK(calls(*k.runner).empty());
    CHECK_FALSE(k.c.elevated());

    k.runner->setStart(ERROR_SUCCESS);
    k.watch.phases.clear();
    k.c.retry();
    k.watch.settle();
    CHECK(text(k.watch.phases) == "uac|running|restarting|testing|done");
}

TEST_CASE("an approval that fails otherwise is a failure with Windows' message") {
    Controller k;
    k.runner->setStart(ERROR_FILE_NOT_FOUND);
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    CHECK(text(k.c.phase()) == "failed");
    CHECK(text(k.c.reason()) == "The system cannot find the file specified.");
}

TEST_CASE("when restart-audio fails the change is done and a Windows restart is offered; nothing is tested") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("restart-audio", {ERROR_SUCCESS, 1, "{\"command\":\"restart-audio\",\"ok\":false,\"reason\":\"timeout\"}", 0});
    k.c.run(QStringLiteral("uninstall"), kCable, {QStringLiteral("uninstall"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|restarting|reboot");
    CHECK(calls(*k.runner) == "uninstall " + kCable.toStdString() + " ; restart-audio");
    QSignalSpy reboot(&k.c, &DevicetoolController::restartWindowsRequested);
    k.c.restartWindows();
    CHECK(reboot.count() == 1);
}

TEST_CASE("a failing test after a change is a failed test") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("test", {ERROR_SUCCESS, 1, "{\"command\":\"test\",\"ok\":false,\"reason\":\"audio client initialization failed\"}", 0});
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|restarting|testing|failed");
    CHECK(text(k.c.kind()) == "test");
    CHECK(text(k.c.reason()) == "Audio client initialization failed.");
}

TEST_CASE("Test runs unelevated and restarts nothing") {
    Controller k;
    k.c.run(QStringLiteral("test"), kCable, {QStringLiteral("test"), kCable});
    k.watch.settle();
    CHECK(text(k.watch.phases) == "running|done");
    CHECK(calls(*k.runner) == "direct:test " + kCable.toStdString());
    CHECK_FALSE(k.c.elevated());
}

TEST_CASE("a broken session is a failure, not a crash") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("install", {ERROR_BROKEN_PIPE, -1, "", 0});
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    k.watch.settle();
    CHECK(text(k.c.phase()) == "failed");
    CHECK(text(k.c.reason()) == "The pipe has been ended.");
}

TEST_CASE("Change in Settings Outputs asks for approval") {
    Controller k;
    k.runner->setStart(ERROR_CANCELLED);
    k.c.requestApproval();
    k.watch.settle();
    CHECK(text(k.watch.phases) == "uac|declined");
    CHECK(text(k.c.kind()) == "approval");
    k.runner->setStart(ERROR_SUCCESS);
    k.c.requestApproval();
    k.watch.settle();
    CHECK(text(k.c.phase()) == "done");
    CHECK(k.c.elevated());
    CHECK(calls(*k.runner).empty());
}

TEST_CASE("apply runs every plan in order, attaches, removes, restarts audio once and tests the outputs devicetool changed") {
    const fs::path box = fs::temp_directory_path() / ("isotone-apply-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(box);
    fs::create_directories(box);
    REQUIRE_FALSE(isotone::compat::is_live_install_path(box));
    { std::ofstream(box / "config.txt", std::ios::binary) << "Include: peace.txt\r\n"; }
    { std::ofstream(box / "Isotone.txt", std::ios::binary) << "Device: {6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}\r\nChannel: all\r\nPreamp: -3 dB\r\n"; }
    const QString previous = AppPaths::compatConfigDir();
    AppPaths::setCompatConfigDir(QString::fromStdWString(box.wstring()));
    // Off is kept in settings: a directory of this test's.
    QTemporaryDir data;
    const QString previous_data = AppPaths::dataDir();
    AppPaths::setDataDir(data.path());

    Controller k;
    QStringList history;
    QObject::connect(&k.c, &DevicetoolController::changed, [&] {
        QStringList row;
        for (auto it = k.c.rowStatus().cbegin(); it != k.c.rowStatus().cend(); ++it) row << it.key().left(3) + QLatin1Char('=') + it.value().toString();
        const QString line = row.join(QLatin1Char(','));
        if (history.isEmpty() || history.back() != line) history << line;
    });
    const QVariantList plans = {
        planChange(facts("not_installed"), QStringLiteral("IsoAPO")),
        planChange(facts("derived-alongside"), QStringLiteral("Equalizer APO")),
        planChange(facts("eapo"), QStringLiteral("Off")),
    };
    k.c.apply(plans);
    k.watch.settle();
    CHECK(text(k.c.kind()) == "apply");
    CHECK(text(k.watch.phases) == "uac|running|restarting|testing|done");
    CHECK(k.c.restarted());
    CHECK(calls(*k.runner) == "install {407cef09-cb03-4063-a26f-2ff82a1c0e4a} ; uninstall " + kCable.toStdString() +
                                 " ; restart-audio ; direct:test {407cef09-cb03-4063-a26f-2ff82a1c0e4a} ; direct:test " +
                                 kCable.toStdString());
    const QVariantMap rows = k.c.rowStatus();
    CHECK(text(rows.value(QStringLiteral("{407cef09-cb03-4063-a26f-2ff82a1c0e4a}")).toString()) == "installed");
    CHECK(text(rows.value(kCable).toString()) == "attached");
    CHECK(text(rows.value(QStringLiteral("{6cd5cc5c-be4c-4f7a-8090-20f6cb934c21}")).toString()) == "removed");
    // Rows go queued, then working, then done, one at a time.
    CHECK(text(history.first()) == "{40=queued,{6c=queued,{79=queued");
    CHECK(history.contains(QStringLiteral("{40=installing,{6c=queued,{79=queued")));
    CHECK(history.contains(QStringLiteral("{40=installed,{6c=queued,{79=removing")));
    CHECK(history.contains(QStringLiteral("{40=installed,{6c=queued,{79=attaching")));
    CHECK(history.contains(QStringLiteral("{40=installed,{6c=removing,{79=attached")));

    std::stringstream config, isotone_file;
    config << std::ifstream(box / "config.txt", std::ios::binary).rdbuf();
    isotone_file << std::ifstream(box / "Isotone.txt", std::ios::binary).rdbuf();
    CHECK(config.str().find("Include: Isotone.txt") != std::string::npos);
    CHECK(config.str().find("Include: peace.txt") != std::string::npos);   // Settings Outputs keeps Peace
    CHECK(isotone_file.str().find("6cd5cc5c") == std::string::npos);

    AppPaths::setDataDir(previous_data);
    AppPaths::setCompatConfigDir(previous);
    fs::remove_all(box);
}

TEST_CASE("a plan that fails marks its row, the rest still run, and the reason is kept") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("install", {ERROR_SUCCESS, 1, "{\"command\":\"install\",\"ok\":false,\"reason\":\"registration checks failed\"}", 0});
    k.runner->answer("uninstall", ok());
    k.c.apply({planChange(facts("not_installed"), QStringLiteral("IsoAPO")), planChange(facts("installed"), QStringLiteral("Off"))});
    k.watch.settle();
    CHECK(text(k.c.phase()) == "failed");
    CHECK(text(k.c.reason()) == "Registration checks failed.");
    CHECK(text(k.c.rowStatus().value(QStringLiteral("{407cef09-cb03-4063-a26f-2ff82a1c0e4a}")).toString()) == "failed");
    CHECK(text(k.c.rowStatus().value(kCable).toString()) == "removed");
    CHECK(calls(*k.runner) == "install {407cef09-cb03-4063-a26f-2ff82a1c0e4a} ; uninstall " + kCable.toStdString() +
                                 " ; restart-audio ; direct:test " + kCable.toStdString());
}

TEST_CASE("nothing starts while an operation is working") {
    Controller k;
    k.runner->setStarted(true);
    k.runner->answer("install", {ERROR_SUCCESS, 0, "{\"ok\":true}", -1});   // held until cancel
    k.c.run(QStringLiteral("install"), kCable, {QStringLiteral("install"), kCable});
    QElapsedTimer t;
    t.start();
    while (k.c.phase() != QLatin1String("running") && t.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    CHECK(k.c.working());
    k.c.run(QStringLiteral("uninstall"), kCable, {QStringLiteral("uninstall"), kCable});
    k.c.requestApproval();
    CHECK(text(k.c.kind()) == "install");
    k.runner->cancel();
}

TEST_CASE("a script sets the runner's answers") {
    ScriptedRunner r;
    CHECK_FALSE(r.load("[]"));
    REQUIRE(r.load(R"({"started": true, "start": {"error": 1223},
                      "commands": {"install": [{"exit": 1, "json": {"reason": "no"}}, {"exit": 0}]}})"));
    CHECK(r.started());
    CHECK(r.start() == ERROR_CANCELLED);
    DevicetoolResult a = r.run({L"install", L"x"});
    CHECK(a.exit_code == 1);
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(a.json)).object().value(QStringLiteral("reason")).toString() == QStringLiteral("no"));
    CHECK(r.run({L"install", L"x"}).exit_code == 0);
    CHECK(r.run({L"install", L"x"}).exit_code == 0);   // the last repeats
    const DevicetoolResult other = r.run({L"restart-audio"});
    CHECK(other.exit_code == 0);
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(other.json)).object().value(QStringLiteral("command")).toString() ==
          QStringLiteral("restart-audio"));
}

// ---------------------------------------------------------------------------
// The real session, unelevated

TEST_CASE("the controller on an unelevated serve: a changing command is exit code 3, shown as a failure") {
    const std::wstring exe = devicetoolPath().toStdWString();
    REQUIRE_MESSAGE(GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES, devicetoolPath().toStdString());
    // An elevated serve would run it for real. CABLE Input's enhancements flag is
    // absent (decisions.md, 2026-09-14), but nothing here relies on that.
    REQUIRE_MESSAGE(!elevated(), "this test sends a changing command and must not run elevated");

    auto runner = std::make_unique<SessionRunner>(exe, false);
    SessionRunner* session = runner.get();
    DevicetoolController c(std::move(runner));
    Watch watch(c);
    c.run(QStringLiteral("repair"), kCable, {QStringLiteral("enable-enhancements"), kCable});
    watch.settle();
    CHECK(text(watch.phases) == "uac|running|failed");
    CHECK(c.elevated());   // the session started, unelevated
    CHECK(text(c.reason()) == "Needs an elevated process; use --dry-run to preview.");
    CHECK(c.details().contains(QStringLiteral("exit 3")));
    CHECK(session->started());

    // Read-only commands work through it, and directly.
    const DevicetoolResult status = session->runDirect({L"status", kCableW});
    CHECK(status.error == ERROR_SUCCESS);
    CHECK(status.exit_code == 0);
    DeviceFacts f;
    CHECK(factsFromStatusJson(QByteArray::fromStdString(status.json), &f));
    CHECK(f.guid == kCable);
}
