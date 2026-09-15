// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The speaker controls on EqSession, without QML: target labels and masks, a
// layout change, solo and test tones against what is saved, bass management,
// distances, groups, the Showing picker and the layout setter.

#include "doctest.h"

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>

#include <windows.h>

#include <cmath>
#include <filesystem>

#include "apppaths.h"
#include "eqsession.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "isotone_file.h"
#include "persisted_state.h"
#include "responsegraph.h"
#include "speakers.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

constexpr uint32_t k71 = 0x63F, k51 = 0x60F;

OutputTarget surround(uint32_t channels = 8, uint32_t mask = k71, Backend backend = Backend::none,
                      std::wstring guid = L"") {
    return OutputTarget{std::move(guid), backend, OutputLayout{channels, mask, 48000}};
}

Band peak(uint32_t id, ChannelMask mask, double fc, double gain) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = gain;
    b.width = 1;
    b.channels = mask;
    return b;
}

QString temp_path(const char* name) {
    return QDir::temp().filePath(QStringLiteral("isotone-speakers-%1-%2").arg(QCoreApplication::applicationPid()).arg(QLatin1String(name)));
}

int row_of(const Speakers& speakers, const char* code) {
    for (int row = 0; row < speakers.rowCount(); ++row)
        if (speakers.data(speakers.index(row), Speakers::CodeRole).toString() == QLatin1String(code)) return row;
    return -1;
}

}  // namespace

TEST_CASE("a band's target on a surround output: its mask and label") {
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("targets.json")));
    QFile::remove(temp_path("targets.json"));
    session.useTarget(surround());
    EqState s;
    s.bands = {peak(1, kAllChannels, 100, 3)};
    session.loadState(&s);
    const auto label = [&] { return session.data(session.index(0), EqSession::TargetRole).toString(); };
    const auto mask = [&] { return session.data(session.index(0), EqSession::ChannelMaskRole).toInt(); };

    CHECK(label() == QStringLiteral("All"));
    session.setChannelMask(0, 0x07);
    CHECK(session.bandAt(0)->channels == 0x07);
    CHECK(label() == QStringLiteral("Front"));
    CHECK(mask() == 0x07);
    CHECK(session.state().layout_channels == 8);   // masks address the output's layout
    CHECK(session.state().layout_speaker_mask == k71);
    session.setChannelMask(0, 0x08);
    CHECK(label() == QStringLiteral("Sub"));
    session.setChannelMask(0, 0x04);
    CHECK(label() == QStringLiteral("Centre"));
    session.setChannelMask(0, 0x41);
    CHECK(label() == QStringLiteral("L SL"));
    session.setChannelMask(0, 0xFF);   // every speaker is all channels
    CHECK(session.bandAt(0)->channels == kAllChannels);
    session.setChannelMask(0, 0x100);  // none of the layout's: nothing changes
    CHECK(session.bandAt(0)->channels == kAllChannels);

    REQUIRE(speakers.addGroup(QStringLiteral("Heights"), {QStringLiteral("SL"), QStringLiteral("SR")}));
    session.setChannelMask(0, 0xC0);
    CHECK(label() == QStringLiteral("Heights"));

    // Stereo keeps its labels.
    session.useTarget(surround(2, 0x3, Backend::none, L"{8f4d2a10-0000-4000-8000-0000000057e0}"));
    session.loadState(&s);
    CHECK(label() == QStringLiteral("L+R"));
    QFile::remove(temp_path("targets.json"));
}

TEST_CASE("a layout change moves the speaker values by speaker role") {
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("remap.json")));
    session.useTarget(surround());
    EqState s;
    s.bands = {peak(1, 0x10, 100, 3), peak(2, 0x80, 200, -3), peak(3, kAllChannels, 300, 1)};
    session.loadState(&s);
    REQUIRE(speakers.rowCount() == 8);
    speakers.setLevel(row_of(speakers, "SL"), -2.0);
    speakers.setDelay(row_of(speakers, "SR"), 1.5);
    speakers.setInverted(row_of(speakers, "LFE"), true);

    session.useTarget(surround(6, k51));   // the same output, now 5.1
    const EqState& now = session.state();
    CHECK(now.layout_channels == 6);
    CHECK(now.bands[0].channels == 0x10);   // RL stands in as SL, channel 4
    CHECK(now.bands[1].channels == 0x20);   // SR, channel 5
    CHECK(now.bands[2].channels == kAllChannels);
    CHECK(now.channel_gain_db[4] == -2.0);  // SL
    CHECK(now.channel_gain_db[6] == 0.0);
    CHECK(now.speakers.delay_ms[5] == 1.5); // SR
    CHECK(now.speakers.inverted == 0x08);   // the LFE is channel 3 on both
    REQUIRE(speakers.rowCount() == 6);
    CHECK(speakers.data(speakers.index(4), Speakers::CodeRole).toString() == QStringLiteral("SL"));
    CHECK(speakers.data(speakers.index(4), Speakers::LevelRole).toDouble() == -2.0);
    CHECK(speakers.data(speakers.index(5), Speakers::DelayRole).toDouble() == 1.5);
    CHECK(session.data(session.index(1), EqSession::TargetRole).toString() == QStringLiteral("Side right"));
    QFile::remove(temp_path("remap.json"));
}

TEST_CASE("a state written for another layout loads moved to the output's") {
    EqSession session;
    session.useTarget(surround());
    EqState s;
    s.layout_channels = 6;
    s.layout_speaker_mask = k51;
    s.bands = {peak(1, 0x30, 100, 3)};   // SL SR on 5.1
    s.channel_gain_db[4] = -3;           // SL
    session.loadState(&s);
    CHECK(session.state().bands[0].channels == 0xC0);
    CHECK(session.state().channel_gain_db[6] == -3);
    CHECK(session.state().channel_gain_db[4] == 0);
    CHECK(session.state().layout_channels == 8);
}

TEST_CASE("test tones and solo reach an Equalizer APO output, and the real state comes back") {
    // What DeviceLink writes, read back from a sandbox Isotone.txt.
    const QString dir = temp_path("compat");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    AppPaths::setCompatConfigDir(dir);
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-0000000c0a7e}";
    const auto written = [&](const std::function<bool(const EqState&)>& want) {
        for (int i = 0; i < 100; ++i) {
            // Shared for delete too: the writer replaces the file while it is read.
            const HANDLE file = CreateFileW(QDir(dir).filePath(QStringLiteral("Isotone.txt")).toStdWString().c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                std::string text(64 * 1024, '\0');
                DWORD read = 0;
                ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
                CloseHandle(file);
                text.resize(read);
                for (const compat::ParsedDevice& d :
                     compat::parse_isotone_file(text, [](const std::string&) { return ChannelLayout{8, k71}; })) {
                    if (d.endpoint_guid.find("8f4d2a10-0000-4000-8000-0000000c0a7e") != std::string::npos && want(d.state)) return true;
                }
            }
            Sleep(30);
        }
        return false;
    };
    {
        EqSession session;
        AppPaths::setCompatConfigDir(QString());
        Speakers speakers(&session);
        speakers.setStore(SpeakerStore(temp_path("compat.json")));
        session.useTarget(surround(8, k71, Backend::equalizer_apo, guid));
        session.addBand(1000, -6);   // committed
        speakers.setUpmix(1);
        CHECK(written([](const EqState& s) { return !s.bypass && s.speakers.upmix == Upmix::All && s.speakers.muted == 0; }));

        speakers.toggleSolo(0);
        CHECK(written([](const EqState& s) { return s.speakers.muted == 0xFE; }));
        speakers.setTestTones(true);
        CHECK(written([](const EqState& s) { return s.bypass && s.speakers.upmix == Upmix::Off && s.speakers.muted == 0xFE; }));
        speakers.setTestTones(false);
        speakers.toggleSolo(0);
        CHECK(written([](const EqState& s) {
            return !s.bypass && s.speakers.upmix == Upmix::All && s.speakers.muted == 0 && s.bands.size() == 1;
        }));
    }
    QDir(dir).removeRecursively();
    QFile::remove(temp_path("compat.json"));
}

TEST_CASE("solo and test tones reach the output and are never saved") {
    wchar_t temp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    const std::filesystem::path dir =
        std::filesystem::path(temp) / (L"isotone-speakers-saved-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-00000050101e}";   // no such endpoint: no region, no audio

    EqSession session;
    session.setSavedStateDir(dir.wstring());
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("solo.json")));
    session.useTarget(surround(8, k71, Backend::native, guid));
    EqState s;
    s.bands = {peak(1, kAllChannels, 1000, 6)};
    s.speakers.upmix = Upmix::All;
    s.speakers.swap_left_right = true;
    s.speakers.muted = 0x80;
    session.loadState(&s);
    const std::wstring path = isotone::win::persisted_state_path(dir.wstring(), guid);
    const auto saved = [&] {
        ParamBlock block{};
        REQUIRE(isotone::win::read_persisted_state(path, &block) == isotone::win::PersistedRead::Loaded);
        EqState out;
        from_param_block(block, &out);
        return out;
    };

    const int c = row_of(speakers, "C");
    speakers.toggleSolo(c);
    CHECK(speakers.soloRow() == c);
    CHECK(session.engineState().speakers.muted == 0xFB);
    CHECK(session.savedState().speakers.muted == 0x80);
    CHECK(speakers.data(speakers.index(0), Speakers::SoloMutedRole).toBool());
    CHECK_FALSE(speakers.data(speakers.index(c), Speakers::SoloMutedRole).toBool());
    CHECK_FALSE(speakers.data(speakers.index(0), Speakers::MutedRole).toBool());

    // A speaker change while soloed saves the mutes without the solo.
    speakers.setLevel(c, 1.5);
    CHECK(saved().speakers.muted == 0x80);
    CHECK(saved().channel_gain_db[2] == doctest::Approx(1.5));
    CHECK(saved().bands.empty());   // no saved state was flat; the unsaved band is not saved with it

    // Small C: its bass goes to the LFE, which solo leaves playing.
    speakers.setSmall(c, true);
    CHECK(session.state().speakers.bass_management);
    CHECK(session.engineState().speakers.muted == 0xF3);
    CHECK(saved().speakers.muted == 0x80);

    // Test tones: bypassed, no upmix or swaps, on top of the solo.
    QSignalSpy failed(&speakers, &Speakers::toneFailed);
    speakers.setTestTones(true);
    CHECK(speakers.testTones());
    CHECK(speakers.playingRow() == 0);
    EqState live = session.engineState();
    CHECK(live.bypass);
    CHECK(live.speakers.upmix == Upmix::Off);
    CHECK_FALSE(live.speakers.swap_left_right);
    CHECK(live.speakers.muted == 0xF3);
    CHECK_FALSE(session.state().bypass);   // the edited state is untouched
    speakers.setLevel(0, -1.0);
    CHECK_FALSE(saved().bypass);
    CHECK(saved().speakers.upmix == Upmix::All);
    CHECK(saved().speakers.swap_left_right);

    speakers.toggleTone(3);
    CHECK(speakers.playingRow() == 3);
    speakers.toggleTone(3);
    CHECK(speakers.playingRow() == -1);
    CHECK(speakers.testTones());

    speakers.setTestTones(false);
    live = session.engineState();
    CHECK_FALSE(live.bypass);
    CHECK(live.speakers.upmix == Upmix::All);
    CHECK(live.speakers.swap_left_right);
    CHECK(live.speakers.muted == 0xF3);   // still soloed

    speakers.endSession();
    CHECK(speakers.soloRow() == -1);
    CHECK(session.engineState().speakers.muted == 0x80);

    SUBCASE("a tone that cannot play turns test tones off and puts the state back") {
        speakers.setTestTones(true);
        for (int i = 0; i < 100 && failed.isEmpty(); ++i) {
            QCoreApplication::processEvents();
            Sleep(20);
        }
        REQUIRE(failed.size() == 1);
        CHECK(failed.first().first().toInt() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        CHECK_FALSE(speakers.testTones());
        CHECK_FALSE(session.engineState().bypass);
    }
    std::filesystem::remove_all(dir);
    QFile::remove(temp_path("solo.json"));
}

TEST_CASE("a failure of a tone already stopped does not end test tones") {
    EqSession session;
    session.setSavedStateDir(QDir::temp().filePath(QStringLiteral("isotone-speakers-stale")).toStdWString());
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("stale.json")));
    session.useTarget(surround(8, k71, Backend::native, L"{8f4d2a10-0000-4000-8000-0000005a1e00}"));   // no such endpoint
    QSignalSpy failed(&speakers, &Speakers::toneFailed);
    speakers.setTestTones(true);   // its tone fails on its thread
    Sleep(1000);                   // the failure is queued by now
    speakers.toggleTone(0);        // paused: that tone is stopped
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
        Sleep(20);
    }
    CHECK(speakers.testTones());
    CHECK(failed.isEmpty());
    speakers.setTestTones(false);
    QDir(QDir::temp().filePath(QStringLiteral("isotone-speakers-stale"))).removeRecursively();
    QFile::remove(temp_path("stale.json"));
}

TEST_CASE("bass management takes its ranges in 10 Hz steps") {
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("bass.json")));
    session.useTarget(surround());
    CHECK(speakers.crossoverHz() == 80);
    CHECK(speakers.lfeLowpassHz() == 120);
    speakers.setCrossoverHz(37);
    CHECK(session.state().speakers.crossover_hz == 40);
    speakers.setCrossoverHz(96);
    CHECK(speakers.crossoverHz() == 100);
    speakers.setCrossoverHz(400);
    CHECK(speakers.crossoverHz() == 250);
    speakers.setLfeLowpassHz(20);
    CHECK(speakers.lfeLowpassHz() == 80);
    speakers.setLfeLowpassHz(144);
    CHECK(speakers.lfeLowpassHz() == 140);
    speakers.setCrossoverHz(NAN);
    CHECK(speakers.crossoverHz() == 250);

    CHECK_FALSE(speakers.bassManagement());
    speakers.setSmall(row_of(speakers, "SL"), true);
    CHECK(speakers.bassManagement());
    CHECK(speakers.smallSpeakers() == 0x40);
    speakers.setSmall(row_of(speakers, "LFE"), true);   // the LFE is never small
    CHECK(speakers.smallSpeakers() == 0x40);
    speakers.setSmall(row_of(speakers, "SL"), false);
    CHECK_FALSE(speakers.bassManagement());

    speakers.setLipSyncMs(-5);
    CHECK(speakers.lipSyncMs() == 0);
    speakers.setLipSyncMs(40);
    CHECK(session.state().speakers.lip_sync_ms == 40);
    speakers.setUpmix(2);
    CHECK(session.state().speakers.upmix == Upmix::NoCentre);
    speakers.setUpmix(7);
    CHECK(session.state().speakers.upmix == Upmix::NoCentre);
    speakers.setLevel(0, 40);
    CHECK(session.state().channel_gain_db[0] == 12);
    QFile::remove(temp_path("bass.json"));
}

TEST_CASE("distances are kept per output and read back from the delays") {
    const QString store = temp_path("distance.json");
    QFile::remove(store);
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-00000000d157}";
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(store));
    session.useTarget(surround(8, k71, Backend::none, guid));
    CHECK(speakers.data(speakers.index(0), Speakers::DistanceRole).toDouble() == kDefaultFarthestM);

    speakers.setDistance(row_of(speakers, "LFE"), 3.40);
    speakers.setDistance(row_of(speakers, "RL"), 2.20);
    CHECK(speakers.data(speakers.index(row_of(speakers, "RL")), Speakers::DelayRole).toDouble() == doctest::Approx(3.4985).epsilon(1e-4));
    CHECK(speakers.data(speakers.index(0), Speakers::DelayRole).toDouble() == doctest::Approx(1.1662).epsilon(1e-4));
    CHECK(speakers.data(speakers.index(row_of(speakers, "RL")), Speakers::DistanceRole).toDouble() == doctest::Approx(2.20));

    // Another session on the same output reads the distances back.
    EqSession other;
    Speakers again(&other);
    again.setStore(SpeakerStore(store));
    other.useTarget(surround(8, k71, Backend::none, guid));
    other.loadState(&session.state());
    CHECK(again.data(again.index(row_of(again, "RL")), Speakers::DistanceRole).toDouble() == doctest::Approx(2.20));
    CHECK(again.data(again.index(row_of(again, "LFE")), Speakers::DistanceRole).toDouble() == doctest::Approx(3.40));

    speakers.setDelay(0, 2.0);
    CHECK(session.state().speakers.delay_ms[0] == 2.0);
    CHECK(speakers.data(speakers.index(0), Speakers::DistanceRole).toDouble() == doctest::Approx(3.40 - 0.686));
    QFile::remove(store);
}

TEST_CASE("speaker groups are added, kept per output, and removed") {
    const QString store = temp_path("groups.json");
    QFile::remove(store);
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-000000009009}";
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(store));
    session.useTarget(surround(8, k71, Backend::none, guid));
    CHECK(speakers.groups().size() == 4);
    CHECK_FALSE(speakers.addGroup(QStringLiteral(""), {QStringLiteral("SL")}));
    CHECK_FALSE(speakers.addGroup(QStringLiteral("front"), {QStringLiteral("SL")}));   // taken
    CHECK_FALSE(speakers.addGroup(QStringLiteral("Tops"), {}));
    CHECK_FALSE(speakers.addGroup(QStringLiteral("Tops"), {QStringLiteral("TFL")}));
    REQUIRE(speakers.addGroup(QStringLiteral(" Heights "), {QStringLiteral("SL"), QStringLiteral("SR")}));
    const QVariantList groups = speakers.groups();
    REQUIRE(groups.size() == 5);
    CHECK(groups[4].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Heights"));
    CHECK(groups[4].toMap()[QStringLiteral("codes")].toString() == QStringLiteral("SL SR"));
    CHECK(groups[4].toMap()[QStringLiteral("mask")].toInt() == 0xC0);
    CHECK(groups[0].toMap()[QStringLiteral("codes")].toString() == QStringLiteral("8 speakers"));

    EqSession other;
    Speakers again(&other);
    again.setStore(SpeakerStore(store));
    other.useTarget(surround(8, k71, Backend::none, guid));
    CHECK(again.groups().size() == 5);
    other.useTarget(surround(8, k71, Backend::none, L"{8f4d2a10-0000-4000-8000-00000000aaaa}"));
    CHECK(again.groups().size() == 4);   // another output's groups

    speakers.setShowing(QStringLiteral("group:Heights"));
    CHECK(session.showingMask() == 0xC0);
    speakers.removeGroup(QStringLiteral("Heights"));
    CHECK(speakers.groups().size() == 4);
    CHECK(speakers.showing() == QStringLiteral("all"));
    CHECK(session.showingMask() == 0);
    QFile::remove(store);
}

TEST_CASE("the Showing picker chooses the channel the graph draws") {
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("showing.json")));
    session.useTarget(surround());
    EqState s;
    s.bands = {peak(1, 0x07, 1000, 3), peak(2, 0x07, 3000, -2), peak(3, 0x08, 60, 6)};
    session.loadState(&s);
    ResponseGraph graph;
    graph.setSession(&session);

    const QVariantList items = speakers.showingItems();
    REQUIRE(items.size() == 1 + 3 + 8);
    CHECK(items[0].toMap()[QStringLiteral("label")].toString() == QStringLiteral("All speakers"));
    CHECK(items[0].toMap()[QStringLiteral("detail")].toString() == QStringLiteral("8"));
    CHECK(items[1].toMap()[QStringLiteral("detail")].toString() == QStringLiteral("L C R"));
    CHECK(items[2].toMap()[QStringLiteral("detail")].toString() == QStringLiteral("SL SR RL RR"));
    CHECK(items[4].toMap()[QStringLiteral("separator")].toBool());
    CHECK(items[7].toMap()[QStringLiteral("label")].toString() == QStringLiteral("Subwoofer"));

    // All speakers: the front channels have the most bands.
    CHECK(speakers.showingLabel() == QStringLiteral("All speakers"));
    CHECK(std::abs(graph.compositeAt(60)) < 0.5);
    CHECK(graph.handleDb(2) == doctest::Approx(6.0).epsilon(0.02));   // the Sub band sits on the LFE's line
    CHECK(graph.onView(2));

    speakers.setShowing(QStringLiteral("speaker:3"));
    CHECK(session.showingMask() == 0x08);
    CHECK(speakers.showingLabel() == QStringLiteral("Subwoofer"));
    CHECK(graph.compositeAt(60) == doctest::Approx(6.0).epsilon(0.02));
    CHECK_FALSE(graph.onView(0));
    CHECK(graph.onView(2));

    speakers.setShowing(QStringLiteral("group:Front"));
    CHECK(session.showingMask() == 0x07);
    CHECK(graph.compositeAt(1000) > 2.5);
    CHECK(std::abs(graph.compositeAt(60)) < 0.5);
    CHECK_FALSE(graph.onView(2));

    speakers.setShowing(QStringLiteral("speaker:12"));
    CHECK(speakers.showing() == QStringLiteral("all"));
    CHECK(session.showingMask() == 0);
    QFile::remove(temp_path("showing.json"));
}

TEST_CASE("changing the layout calls the layout setter for the output, with test tones stopped") {
    EqSession session;
    Speakers speakers(&session);
    speakers.setStore(SpeakerStore(temp_path("layout.json")));
    std::wstring called_guid;
    int calls = 0;
    devices::SpeakerLayout called_layout = devices::SpeakerLayout::stereo;
    speakers.setLayoutSetter([&](const std::wstring& guid, devices::SpeakerLayout layout) {
        ++calls;
        called_guid = guid;
        called_layout = layout;
        return S_OK;
    });
    CHECK(speakers.setLayout(QStringLiteral("5.1")) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));   // no output
    CHECK(calls == 0);

    session.useTarget(surround(8, k71, Backend::none, L"{8f4d2a10-0000-4000-8000-00000000cafe}"));
    CHECK(speakers.layoutName() == QStringLiteral("7.1"));
    CHECK(speakers.layoutChannels(QStringLiteral("5.1")) == 6);
    CHECK(speakers.layoutChannels(QStringLiteral("Stereo")) == 2);
    speakers.setTestTones(true);
    CHECK(speakers.setLayout(QStringLiteral("5.1")) == S_OK);
    CHECK(calls == 1);
    CHECK(called_guid == L"{8f4d2a10-0000-4000-8000-00000000cafe}");
    CHECK(called_layout == devices::SpeakerLayout::five_point_one);
    CHECK_FALSE(speakers.testTones());
    CHECK(speakers.setLayout(QStringLiteral("9.1")) == E_INVALIDARG);
    CHECK(calls == 1);
    QFile::remove(temp_path("layout.json"));
}
