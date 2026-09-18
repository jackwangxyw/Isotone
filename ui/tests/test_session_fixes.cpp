// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Fixes after the stage 4 review: solo and test tones against output switches,
// layout changes and preset saves, undo across layouts and of speaker changes,
// Auto preamp with EQ off, the 64 band limit, the order and place of saved state
// writes, and removing a preset. Outputs are Local\ regions the test creates and
// the self test's saved-state directory, never a real output.

#include "doctest.h"

#include <windows.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <memory>

#include "eqsession.h"
#include "importpreview.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "persisted_state.h"
#include "presets.h"
#include "presetstore.h"
#include "shared_mapping.h"
#include "speakers.h"

using namespace isotone;
using isotone::ui::Backend;
using isotone::ui::OutputLayout;
using isotone::ui::OutputTarget;

namespace {

constexpr uint32_t k71 = 0x63F, k51 = 0x60F;

std::wstring fixNamespace(const char* what) {
    return L"Local\\IsotoneFixTest." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::wstring(what, what + std::strlen(what)) + L".";
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

EqState region(isotone::win::SharedMapping& m) {
    EqState s;
    from_param_block(*m.params(), &s);
    return s;
}

int row_of(const Speakers& speakers, const char* code) {
    for (int row = 0; row < speakers.rowCount(); ++row)
        if (speakers.data(speakers.index(row), Speakers::CodeRole).toString() == QLatin1String(code)) return row;
    return -1;
}

double distance(const Speakers& speakers, int row) {
    return speakers.data(speakers.index(row), Speakers::DistanceRole).toDouble();
}

// Two native outputs whose regions the test holds, as a running engine would.
struct NativeRig {
    QTemporaryDir data;
    QTemporaryDir compat;
    std::wstring space;
    OutputTarget a, b;
    isotone::win::SharedMapping ra, rb;
    std::unique_ptr<EqSession> session;
    std::unique_ptr<Speakers> speakers;
    std::unique_ptr<Presets> presets;
    explicit NativeRig(const char* name, uint32_t channels = 8, uint32_t mask = k71) : space(fixNamespace(name)) {
        a = OutputTarget{"{8f4d2a10-0000-4000-8000-00000000f001}", Backend::native, OutputLayout{channels, mask, 48000}};
        b = OutputTarget{"{8f4d2a10-0000-4000-8000-00000000f002}", Backend::native, OutputLayout{channels, mask, 48000}};
        REQUIRE(ra.create_or_open(isotone::win::mapping_name(space.c_str(), isotone::ui::widen_id(a.guid))) == ERROR_SUCCESS);
        REQUIRE(rb.create_or_open(isotone::win::mapping_name(space.c_str(), isotone::ui::widen_id(b.guid))) == ERROR_SUCCESS);
        session = std::make_unique<EqSession>(std::make_unique<ui::DeviceLink>(space, compat.path().toStdWString()));
        speakers = std::make_unique<Speakers>(session.get());
        speakers->setStore(SpeakerStore(QDir(data.path()).filePath(QStringLiteral("speakers.json"))));
        std::vector<Presets::OutputInfo> outs = {{a, QStringLiteral("A")}, {b, QStringLiteral("B")}};
        presets = std::make_unique<Presets>(data.path(), session.get(), [outs] { return outs; }, space,
                                            compat.path().toStdWString());
    }
    ~NativeRig() {
        presets.reset();
        speakers.reset();
        session.reset();
        for (const OutputTarget& t : {a, b}) std::filesystem::remove(path(t));
    }
    static std::wstring path(const OutputTarget& t) {
        return win::persisted_state_path(win::persisted_state_dir(true), isotone::ui::widen_id(t.guid));
    }
    static EqState saved(const OutputTarget& t) {
        ParamBlock block{};
        EqState out;
        if (win::read_persisted_state(path(t), &block) == win::PersistedRead::Loaded) from_param_block(block, &out);
        return out;
    }
};

}  // namespace

TEST_CASE("fixes: switching output leaves no solo or test tones on the old output") {
    NativeRig rig("switch");
    rig.session->useTarget(rig.a);
    rig.speakers->toggleSolo(row_of(*rig.speakers, "C"));
    rig.speakers->setTestTones(true);
    REQUIRE(region(rig.ra).speakers.muted != 0);
    REQUIRE(region(rig.ra).bypass);

    rig.session->useTarget(rig.b);
    CHECK(rig.speakers->soloRow() == -1);
    CHECK_FALSE(rig.speakers->testTones());
    CHECK(region(rig.ra).speakers.muted == 0);
    CHECK_FALSE(region(rig.ra).bypass);
    CHECK(region(rig.rb).speakers.muted == 0);

    // Back on A: the solo is not read as A's own speaker mute, nor saved as one.
    rig.session->useTarget(rig.a);
    CHECK(rig.session->state().speakers.muted == 0);
    CHECK_FALSE(rig.session->state().bypass);
    rig.speakers->setLevel(row_of(*rig.speakers, "L"), -1.0);
    CHECK(NativeRig::saved(rig.a).speakers.muted == 0);
}

TEST_CASE("fixes: a layout change never writes the old layout's solo mask") {
    NativeRig rig("layoutsolo");
    rig.session->useTarget(rig.a);
    rig.speakers->toggleSolo(row_of(*rig.speakers, "C"));
    REQUIRE(region(rig.ra).speakers.muted != 0);
    std::vector<std::pair<uint32_t, ChannelMask>> writes;
    QObject::connect(rig.session.get(), &EqSession::committed, [&] {
        const EqState r = region(rig.ra);
        writes.push_back({r.layout_channels, r.speakers.muted});
    });
    OutputTarget stereo = rig.a;
    stereo.layout = OutputLayout{2, 0x3, 48000};
    rig.session->useTarget(stereo);
    REQUIRE_FALSE(writes.empty());
    for (const auto& [channels, muted] : writes) {
        CHECK(channels == 2);
        CHECK((muted & 0x3) != 0x3);   // never both speakers of the stereo output muted
    }
    CHECK(region(rig.ra).speakers.muted == 0);
}

TEST_CASE("fixes: saving or loading a preset keeps solo on the output") {
    NativeRig rig("solo");
    rig.session->useTarget(rig.a);
    const int c = row_of(*rig.speakers, "C");
    rig.speakers->toggleSolo(c);
    const ChannelMask solo = rig.session->engineState().speakers.muted;
    REQUIRE(solo != 0);
    REQUIRE(region(rig.ra).speakers.muted == solo);

    REQUIRE(rig.presets->saveAs(QStringLiteral("Room")) == QStringLiteral("Room"));
    CHECK(rig.speakers->soloRow() == c);
    CHECK(region(rig.ra).speakers.muted == solo);
    CHECK(NativeRig::saved(rig.a).speakers.muted == 0);   // the file never has the solo

    rig.session->addBand(100, -3);
    rig.presets->save();
    CHECK(region(rig.ra).speakers.muted == solo);
    CHECK(NativeRig::saved(rig.a).speakers.muted == 0);
    CHECK(NativeRig::saved(rig.a).bands.size() == 1);

    {
        PresetStore store(rig.data.path());
        REQUIRE_FALSE(store.add(QStringLiteral("Other"), EqState{}).isEmpty());
    }
    rig.presets.reset();
    rig.presets = std::make_unique<Presets>(rig.data.path(), rig.session.get(),
                                            [&] { return std::vector<Presets::OutputInfo>{{rig.a, QStringLiteral("A")}}; },
                                            rig.space, rig.compat.path().toStdWString());
    rig.presets->load(QStringLiteral("Other"));
    REQUIRE(rig.session->rowCount() == 0);
    CHECK(region(rig.ra).speakers.muted == solo);
    CHECK(NativeRig::saved(rig.a).speakers.muted == 0);
}

TEST_CASE("fixes: a layout change is not an undo step and clears the history") {
    EqSession s;   // backend none: nothing is written anywhere
    const std::string guid = "{8f4d2a10-0000-4000-8000-00000000f010}";
    s.useTarget(OutputTarget{guid, Backend::none, OutputLayout{8, k71, 48000}});
    EqState st;
    st.layout_channels = 8;
    st.layout_speaker_mask = k71;
    st.bands = {peak(1, 0x40, 1000, -3)};   // SL on 7.1 (channel 6)
    st.channel_gain_db[6] = -4;             // SL trim
    s.loadState(&st);
    s.addBand(2000, 2);                     // an older step
    REQUIRE(s.canUndo());

    QSignalSpy committed(&s, &EqSession::committed);
    s.useTarget(OutputTarget{guid, Backend::none, OutputLayout{6, k51, 48000}});   // the same output, now 5.1
    CHECK(committed.size() == 1);   // the remap is written
    CHECK(s.state().bands[0].channels == 0x10);   // SL is channel 4 on 5.1
    CHECK_FALSE(s.canUndo());
    CHECK_FALSE(s.canRedo());
    s.undo();
    CHECK(s.state().layout_channels == 6);
    CHECK(s.state().bands.size() == 2);
    const EqState engine = s.engineState();
    CHECK(engine.layout_channels == 6);
    CHECK(band_affects_channel(engine.bands[0], 4));
    CHECK(engine.channel_gain_db[4] == -4);
    // An edit after it is undone back to the remapped state, not the old layout's.
    s.setGain(0, -6);
    s.finishEdit();
    s.undo();
    CHECK(s.state().layout_channels == 6);
    CHECK(s.state().bands[0].gain_db == -3);
    CHECK(s.state().channel_gain_db[4] == -4);
}

TEST_CASE("fixes: Auto preamp is computed as EQ on plays, whatever the EQ toggle") {
    EqSession s;   // backend none
    EqState st;
    st.bands = {peak(1, kAllChannels, 1000, 6)};
    s.loadState(&st);
    s.setAutoPreamp(false);
    s.setAutoPreamp(true);
    const double on = s.preampDb();
    REQUIRE(on < -5.0);
    s.setEqOn(false);
    s.setGain(0, 6.5);
    s.finishEdit();
    s.setGain(0, 6.0);
    s.finishEdit();
    CHECK(s.preampDb() == doctest::Approx(on).epsilon(1e-3));
    s.setEqOn(true);
    CHECK(s.preampDb() == doctest::Approx(on).epsilon(1e-3));
}

TEST_CASE("fixes: an output left with EQ off comes back in Auto and not modified") {
    NativeRig rig("bypass", 2, 0x3);
    rig.session->useTarget(rig.a);
    rig.session->addBand(1000, 6);
    REQUIRE(rig.session->autoPreamp());
    REQUIRE(rig.presets->saveAs(QStringLiteral("Boost")) == QStringLiteral("Boost"));
    REQUIRE_FALSE(rig.presets->modified());
    rig.session->setEqOn(false);
    CHECK_FALSE(rig.presets->modified());
    rig.session->useTarget(rig.b);
    rig.session->useTarget(rig.a);
    CHECK(rig.session->autoPreamp());
    CHECK_FALSE(rig.presets->modified());

    // Another output assigned the preset gets Auto's value as EQ on plays it, though its EQ is off.
    rig.session->setEqOn(true);
    rig.session->useTarget(rig.b);
    rig.session->setEqOn(false);
    rig.session->useTarget(rig.a);
    rig.presets->assign(QString::fromStdString(rig.b.guid), QStringLiteral("Boost"));
    const EqState other = region(rig.rb);
    REQUIRE(other.bypass);
    CHECK(other.preamp_db == doctest::Approx(rig.session->preampDb()).epsilon(1e-4));
}

TEST_CASE("fixes: undoing or redoing a speaker change saves the speaker setup") {
    NativeRig rig("undospk");
    rig.session->useTarget(rig.a);
    const int c = row_of(*rig.speakers, "C");
    rig.speakers->setLevel(c, -5.0);
    CHECK(NativeRig::saved(rig.a).channel_gain_db[2] == doctest::Approx(-5.0));
    rig.session->undo();
    CHECK(rig.session->state().channel_gain_db[2] == 0.0);
    CHECK(region(rig.ra).channel_gain_db[2] == 0.0);
    CHECK(NativeRig::saved(rig.a).channel_gain_db[2] == 0.0);
    rig.session->redo();
    CHECK(region(rig.ra).channel_gain_db[2] == doctest::Approx(-5.0));
    CHECK(NativeRig::saved(rig.a).channel_gain_db[2] == doctest::Approx(-5.0));
}

TEST_CASE("fixes: undo and redo of a distance change are exact") {
    NativeRig rig("farthest");
    rig.session->useTarget(rig.a);
    const int l = row_of(*rig.speakers, "L"), r = row_of(*rig.speakers, "R");
    CHECK(distance(*rig.speakers, r) == doctest::Approx(3.0));
    rig.speakers->setDistance(l, 5.0);
    CHECK(distance(*rig.speakers, l) == doctest::Approx(5.0));
    CHECK(distance(*rig.speakers, r) == doctest::Approx(3.0));

    rig.session->undo();
    CHECK(distance(*rig.speakers, r) == doctest::Approx(3.0));
    CHECK(distance(*rig.speakers, l) == doctest::Approx(3.0));
    {
        // The store follows: another session on the output reads the same.
        EqSession other;
        Speakers again(&other);
        again.setStore(SpeakerStore(QDir(rig.data.path()).filePath(QStringLiteral("speakers.json"))));
        other.useTarget(OutputTarget{rig.a.guid, Backend::none, rig.a.layout});
        other.loadState(&rig.session->state());
        CHECK(distance(again, row_of(again, "L")) == doctest::Approx(3.0));
    }

    rig.session->redo();
    CHECK(distance(*rig.speakers, l) == doctest::Approx(5.0));
    CHECK(distance(*rig.speakers, r) == doctest::Approx(3.0));
    rig.session->undo();
    CHECK(distance(*rig.speakers, l) == doctest::Approx(3.0));
    CHECK_FALSE(rig.session->canUndo());
}

TEST_CASE("fixes: import keeps 64 filters and lists the rest as skipped") {
    QTemporaryDir data;
    EqSession s;   // backend none
    Presets presets(data.path(), &s, [] { return std::vector<Presets::OutputInfo>{}; }, fixNamespace("import"),
                    data.path().toStdWString());
    std::string text = "Preamp: -3 dB\n";
    for (int i = 1; i <= 70; ++i)
        text += "Filter " + std::to_string(i) + ": ON PK Fc " + std::to_string(20 + i * 100) + " Hz Gain -1 dB Q 1\n";
    const QString file = QDir(data.path()).filePath(QStringLiteral("big.txt"));
    {
        QFile f(file);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(text.c_str());
    }
    std::unique_ptr<ImportPreview> preview(presets.openImport(QUrl::fromLocalFile(file)));
    REQUIRE(preview);
    CHECK(preview->filterCount() == 64);
    const QVariantList skipped = preview->skipped();
    REQUIRE(skipped.size() == 6);
    for (int i = 0; i < 6; ++i) {
        const QVariantMap m = skipped[i].toMap();
        CHECK(m.value(QStringLiteral("line")).toInt() == 66 + i);   // filter 65 is on line 66
        CHECK(m.value(QStringLiteral("text")).toString() ==
              QStringLiteral("Filter %1: ON PK Fc %2 Hz Gain -1 dB Q 1").arg(65 + i).arg(20 + (65 + i) * 100));
    }
    presets.importPreset(preview.get(), QStringLiteral("Big"));
    CHECK(s.rowCount() == static_cast<int>(kParamMaxBands));
    CHECK(s.state().bands.back().fc == 20 + 64 * 100);
    ParamBlock block{};
    CHECK(to_param_block(s.engineState(), &block));
}

TEST_CASE("fixes: a native output never gets more than 64 bands") {
    NativeRig rig("bands", 2, 0x3);
    rig.session->useTarget(rig.a);
    EqState big;
    big.auto_preamp = true;
    // Cuts, then six boosts past the limit: Auto for the first 64 is 0 dB.
    for (uint32_t i = 1; i <= 70; ++i) big.bands.push_back(peak(i, kAllChannels, 20.0 + i * 100, i <= 64 ? -1 : 10));

    rig.session->loadState(&big);
    CHECK(rig.session->rowCount() == static_cast<int>(kParamMaxBands));
    CHECK(rig.session->state().bands.back().id == kParamMaxBands);

    // A stored preset over the limit loads its first 64 bands, and is modified: the output does not play all of it.
    rig.session->loadState(nullptr);
    {
        PresetStore store(rig.data.path());
        REQUIRE_FALSE(store.add(QStringLiteral("Big"), eq_part(big, 2, 0x3)).isEmpty());
    }
    rig.presets = std::make_unique<Presets>(rig.data.path(), rig.session.get(), [&] {
        return std::vector<Presets::OutputInfo>{{rig.a, QStringLiteral("A")}, {rig.b, QStringLiteral("B")}};
    }, rig.space, rig.compat.path().toStdWString());
    rig.presets->load(QStringLiteral("Big"));
    CHECK(rig.session->rowCount() == static_cast<int>(kParamMaxBands));
    CHECK(rig.ra.params()->band_count == kParamMaxBands);
    CHECK(rig.session->preampDb() == 0.0);
    CHECK(rig.presets->modified());
    ParamBlock block{};
    CHECK(to_param_block(rig.session->engineState(), &block));

    // Another output assigned it gets the same 64 bands, and Auto's value for them.
    rig.presets->assign(QString::fromStdString(rig.b.guid), QStringLiteral("Big"));
    CHECK(rig.rb.params()->band_count == kParamMaxBands);
    CHECK(region(rig.rb).preamp_db == 0.0);
}

TEST_CASE("fixes: a speaker change writes the saved state before the region, in the self test's directory") {
    NativeRig rig("order");
    rig.session->useTarget(rig.a);
    const int c = row_of(*rig.speakers, "C");
    std::filesystem::remove(NativeRig::path(rig.a));
    std::vector<double> saved_at_commit;
    QObject::connect(rig.session.get(), &EqSession::committed,
                     [&] { saved_at_commit.push_back(NativeRig::saved(rig.a).channel_gain_db[2]); });
    rig.speakers->setLevel(c, -2.5);
    REQUIRE(saved_at_commit.size() == 1);
    CHECK(saved_at_commit[0] == doctest::Approx(-2.5));   // the file had it when the region was written
    CHECK(rig.session->savedStatePath() == NativeRig::path(rig.a));
}

TEST_CASE("fixes: removing a preset whose assignments cannot be written still removes it everywhere") {
    NativeRig rig("remove", 2, 0x3);
    rig.session->useTarget(rig.a);
    rig.session->addBand(1000, -3);
    REQUIRE(rig.presets->saveAs(QStringLiteral("Gone")) == QStringLiteral("Gone"));
    REQUIRE(rig.presets->currentName() == QStringLiteral("Gone"));
    // outputs.json cannot be replaced: a directory holds its name.
    const QString outputs = QDir(rig.data.path()).filePath(QStringLiteral("outputs.json"));
    REQUIRE(QFile::remove(outputs));
    REQUIRE(QDir().mkdir(outputs));

    QSignalSpy count(rig.presets.get(), &Presets::countChanged);
    rig.presets->remove(QStringLiteral("Gone"));
    CHECK(rig.presets->names().isEmpty());
    CHECK(count.size() == 1);
    CHECK(rig.presets->currentName() == QStringLiteral("Untitled"));
    CHECK(rig.presets->assignedName(QString::fromStdString(rig.a.guid)).isEmpty());
    // What is on disk reads the same.
    PresetStore again(rig.data.path());
    CHECK(again.presets().empty());
    CHECK(again.byId(again.assignment(QString::fromStdString(rig.a.guid))) == nullptr);
    QDir(outputs).removeRecursively();
}
