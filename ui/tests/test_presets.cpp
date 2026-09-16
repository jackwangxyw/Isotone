// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Presets: the files, the assignments, and loading and saving on outputs that
// are Local\ regions and a sandbox Equalizer APO directory, never a real output.

#include "doctest.h"

#include <windows.h>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <limits>

#include "eqsession.h"
#include "importpreview.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "persisted_state.h"
#include "presets.h"
#include "presetstore.h"
#include "shared_mapping.h"

using namespace isotone;
using isotone::ui::Backend;
using isotone::ui::OutputLayout;
using isotone::ui::OutputTarget;

namespace {

Band band(uint32_t id, FilterType type, double fc, double gain, double width, WidthMode mode) {
    Band b;
    b.id = id;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain;
    b.width = width;
    b.width_mode = mode;
    return b;
}

bool same_band(const Band& a, const Band& b) {
    return a.id == b.id && a.type == b.type && a.fc == b.fc && a.gain_db == b.gain_db && a.width == b.width &&
           a.width_mode == b.width_mode && a.shelf_corner == b.shelf_corner && a.channels == b.channels &&
           a.enabled == b.enabled;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

// A namespace of our own, so no real IsoAPO region is reached.
std::wstring testNamespace(const char* what) {
    return L"Local\\IsotonePresetTest." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::wstring(what, what + std::strlen(what)) + L".";
}

std::unique_ptr<ui::DeviceLink> link(const std::wstring& ns, const QString& compat) {
    return std::make_unique<ui::DeviceLink>(ns, compat.toStdWString());
}

// What a region holds.
EqState regionState(isotone::win::SharedMapping& m) {
    EqState s;
    from_param_block(*m.params(), &s);
    return s;
}

EqState peq(std::vector<Band> bands, double preamp = 0.0, bool autoPreamp = false) {
    EqState s;
    s.bands = std::move(bands);
    s.preamp_db = preamp;
    s.auto_preamp = autoPreamp;
    s.layout_channels = 2;
    s.layout_speaker_mask = 0x3;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Files

TEST_CASE("presets: a preset's file keeps every field of every band exactly") {
    EqState eq;
    eq.preamp_db = -7.123456789012345;
    eq.auto_preamp = true;
    eq.layout_channels = 8;
    eq.layout_speaker_mask = 0x63F;
    const FilterType types[] = {FilterType::Peaking, FilterType::LowPass, FilterType::HighPass, FilterType::BandPass,
                                FilterType::Notch,   FilterType::AllPass, FilterType::LowShelf, FilterType::HighShelf};
    const WidthMode modes[] = {WidthMode::Q, WidthMode::BandwidthOct, WidthMode::SlopeDb};
    uint32_t id = 7;
    for (FilterType t : types) {
        for (WidthMode m : modes) {
            Band b = band(id++, t, 1000.0 / 3.0 + id, -2.0 / 3.0 * id, 0.1 + 1.0 / 7.0, m);
            b.shelf_corner = (id % 2) == 0;
            b.channels = id % 3 == 0 ? kAllChannels : (0x80000000u | id);
            b.enabled = (id % 4) != 0;
            eq.bands.push_back(b);
        }
    }
    const QByteArray bytes = presetfile::write(QStringLiteral("HD 650 · tuned"), eq);
    QString name;
    EqState back;
    REQUIRE(presetfile::read(bytes, &name, &back));
    CHECK(name == QStringLiteral("HD 650 · tuned"));
    CHECK(back.preamp_db == eq.preamp_db);
    CHECK(back.auto_preamp);
    CHECK(back.layout_channels == 8);
    CHECK(back.layout_speaker_mask == 0x63F);
    REQUIRE(back.bands.size() == eq.bands.size());
    for (size_t i = 0; i < eq.bands.size(); ++i) {
        CAPTURE(i);
        CHECK(same_band(back.bands[i], eq.bands[i]));
    }
    // Only the EQ part: the rest of a state is the output's.
    CHECK_FALSE(back.bypass);
    CHECK_FALSE(back.mute);
    CHECK(back.channel_gain_db[0] == 0.0);
}

TEST_CASE("presets: a file that is not a whole preset is not read") {
    EqState eq = peq({band(1, FilterType::Peaking, 1000, -3, 1.41, WidthMode::Q)}, -3.0);
    const QByteArray good = presetfile::write(QStringLiteral("A"), eq);
    QString name = QStringLiteral("untouched");
    EqState out;
    const auto refused = [&](QByteArray bytes) {
        return !presetfile::read(bytes, &name, &out) && name == QStringLiteral("untouched") && out.bands.empty();
    };
    CHECK(refused(""));
    CHECK(refused("not json"));
    CHECK(refused("[1, 2]"));
    CHECK(refused(good.left(good.size() / 2)));
    const auto with = [&](const char* from, const char* to) {
        QByteArray b = good;
        REQUIRE(b.contains(from));
        return b.replace(from, to);
    };
    CHECK(refused(with("\"version\": 1", "\"version\": 2")));   // a newer schema
    CHECK(refused(with("\"format\": \"isotone-preset\"", "\"format\": \"other\"")));
    CHECK(refused(with("\"name\": \"A\"", "\"name\": \"\"")));
    CHECK(refused(with("\"type\": \"peaking\"", "\"type\": \"wobble\"")));
    CHECK(refused(with("\"widthMode\": \"q\"", "\"widthMode\": 3")));
    CHECK(refused(with("\"fc\": 1000", "\"fc\": \"1000\"")));
    CHECK(refused(with("\"fc\": 1000", "\"fc\": -5")));
    CHECK(refused(with("\"width\": 1.41", "\"width\": 0")));
    CHECK(refused(with("\"gainDb\": -3", "\"gainDb\": null")));
    CHECK(refused(with("\"channels\": 0", "\"channels\": -1")));
    CHECK(refused(with("\"channels\": 0", "\"channels\": 4294967296")));
    CHECK(refused(with("\"id\": 1", "\"id\": 1.5")));
    CHECK(refused(with("\"enabled\": true", "\"enabled\": 1")));
    CHECK(refused(with("\"preampDb\": -3", "\"preampDb\": 1e999")));
    CHECK(presetfile::read(good, &name, &out));
}

TEST_CASE("presets: the store skips unreadable files, sorts by name and keeps names unique") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    {
        PresetStore store(dir.path());
        CHECK(store.presets().empty());
        const EqState eq = peq({band(1, FilterType::Peaking, 1000, -3, 1.41, WidthMode::Q)});
        CHECK_FALSE(store.add(QStringLiteral("Late night"), eq).isEmpty());
        CHECK_FALSE(store.add(QStringLiteral("flat"), eq).isEmpty());
        CHECK_FALSE(store.add(QStringLiteral("Preset 10"), eq).isEmpty());
        CHECK_FALSE(store.add(QStringLiteral("Preset 9"), eq).isEmpty());
        // A clash takes the next number.
        const QString id = store.add(QStringLiteral("  Late night "), eq);
        REQUIRE(store.byId(id));
        CHECK(store.byId(id)->name == QStringLiteral("Late night 2"));
        CHECK(store.uniqueName(QStringLiteral("Late night 2")) == QStringLiteral("Late night 3"));
        CHECK(store.uniqueName(QStringLiteral("Late night 2"), id) == QStringLiteral("Late night 2"));
        CHECK(store.uniqueName(QStringLiteral("LATE NIGHT")) == QStringLiteral("LATE NIGHT 3"));
        CHECK(store.uniqueName(QStringLiteral("New")) == QStringLiteral("New"));
        // Renaming onto another's name takes a number too; onto its own, nothing.
        const QString flat = store.byName(QStringLiteral("flat"))->id;
        CHECK(store.rename(flat, QStringLiteral("Late night")) == QStringLiteral("Late night 3"));
        CHECK(store.rename(flat, QStringLiteral("Late night 3")) == QStringLiteral("Late night 3"));
        CHECK(store.rename(flat, QStringLiteral("  ")).isEmpty());
        CHECK(store.rename(flat, QStringLiteral("Flat")) == QStringLiteral("Flat"));
    }
    writeFile(QDir(dir.path()).filePath(QStringLiteral("presets/broken.json")), "{\"format\": \"isotone-preset\",");
    writeFile(QDir(dir.path()).filePath(QStringLiteral("presets/notes.txt")), "not a preset");
    // A copied file with a name already taken.
    const QString some = QDir(dir.path()).filePath(QStringLiteral("presets"));
    QString copied;
    for (const QString& f : QDir(some).entryList({QStringLiteral("*.json")})) {
        if (readFile(QDir(some).filePath(f)).contains("\"Flat\"")) copied = QDir(some).filePath(f);
    }
    REQUIRE_FALSE(copied.isEmpty());
    REQUIRE(QFile::copy(copied, QDir(some).filePath(QStringLiteral("zz-copy.json"))));

    PresetStore again(dir.path());
    QStringList names;
    for (const auto& p : again.presets()) names << p.name;
    CHECK(names == QStringList{QStringLiteral("Flat"), QStringLiteral("Flat 2"), QStringLiteral("Late night"),
                               QStringLiteral("Late night 2"), QStringLiteral("Preset 9"), QStringLiteral("Preset 10")});
    REQUIRE(again.byName(QStringLiteral("Late night")));
    CHECK(again.byName(QStringLiteral("late night")) == nullptr);   // names are exact
    CHECK(again.byName(QStringLiteral("Late night"))->eq.bands.size() == 1);

    // Writes leave no temporary file behind.
    for (const QString& f : QDir(some).entryList(QDir::Files))
        CHECK((f.endsWith(QStringLiteral(".json")) || f == QStringLiteral("notes.txt")));
}

TEST_CASE("presets: assignments and output names persist, and go with a removed preset") {
    QTemporaryDir dir;
    const QString a = QStringLiteral("{8f4d2a10-0000-4000-8000-00000000000a}");
    const QString b = QStringLiteral("{8f4d2a10-0000-4000-8000-00000000000b}");
    QString id;
    {
        PresetStore store(dir.path());
        id = store.add(QStringLiteral("Desk"), peq({}));
        CHECK(store.assign(a, id, QStringLiteral("Speakers")));
        CHECK(store.assign(b, id, QStringLiteral("Headphones")));
    }
    {
        PresetStore store(dir.path());
        CHECK(store.assignment(a) == id);
        CHECK(store.outputName(b) == QStringLiteral("Headphones"));
        CHECK(store.assignedTo(id).size() == 2);
        CHECK(store.assign(b, QString(), QStringLiteral("Headphones")));   // unassigned
        CHECK(store.setOutputName(a, QStringLiteral("Speakers (Realtek)")));
    }
    {
        PresetStore store(dir.path());
        CHECK(store.assignment(b).isEmpty());
        CHECK(store.outputName(a) == QStringLiteral("Speakers (Realtek)"));
        CHECK(store.remove(id));
        CHECK(store.assignment(a).isEmpty());
        CHECK(store.presets().empty());
    }
    PresetStore store(dir.path());
    CHECK(store.assignment(a).isEmpty());
    CHECK(store.presets().empty());

    // A broken outputs.json reads as no assignments.
    writeFile(QDir(dir.path()).filePath(QStringLiteral("outputs.json")), "{\"version\": 1, \"outputs\": [");
    PresetStore broken(dir.path());
    CHECK(broken.assignment(a).isEmpty());
}

TEST_CASE("presets: same_eq ignores ids and float32 rounding, and sees every other change") {
    EqState a = peq({band(1, FilterType::Peaking, 1000.123, -3.21, 1.41, WidthMode::Q)}, -4.0);
    EqState b = a;
    b.bands[0].id = 9;
    b.bands[0].fc = static_cast<float>(a.bands[0].fc);
    b.bands[0].gain_db = static_cast<float>(a.bands[0].gain_db);
    b.bands[0].width = static_cast<float>(a.bands[0].width);
    b.preamp_db = static_cast<float>(a.preamp_db);
    CHECK(same_eq(a, b));
    EqState c = b;
    c.bands[0].gain_db += 0.01;
    CHECK_FALSE(same_eq(a, c));
    c = b;
    c.bands[0].enabled = false;
    CHECK_FALSE(same_eq(a, c));
    c = b;
    c.bands[0].channels = 0x2;
    CHECK_FALSE(same_eq(a, c));
    c = b;
    c.bands.push_back(c.bands[0]);
    CHECK_FALSE(same_eq(a, c));
    c = b;
    c.preamp_db = -5;
    CHECK_FALSE(same_eq(a, c));
    c.auto_preamp = a.auto_preamp = true;   // Auto's preamp follows the output
    CHECK(same_eq(a, c));
    c.auto_preamp = false;
    CHECK_FALSE(same_eq(a, c));
    // The same bands for another layout.
    EqState surround = b;
    surround.layout_channels = 8;
    surround.layout_speaker_mask = 0x63F;
    surround.bands[0].channels = 0x2;
    EqState stereo = a;
    stereo.auto_preamp = true;
    surround.auto_preamp = true;
    stereo.bands[0].channels = 0x2;
    CHECK(same_eq(stereo, surround));
}

// ---------------------------------------------------------------------------
// Presets on outputs

namespace {

struct Rig {
    QTemporaryDir data;
    QTemporaryDir compat;
    std::wstring ns;
    OutputTarget a, b, c;
    std::vector<Presets::OutputInfo> outputs;
    std::unique_ptr<EqSession> session;
    std::unique_ptr<Presets> presets;

    explicit Rig(const char* name) : ns(testNamespace(name)) {
        a = OutputTarget{L"{8f4d2a10-0000-4000-8000-0000000000a1}", Backend::native, OutputLayout{2, 0x3, 48000}};
        b = OutputTarget{L"{8f4d2a10-0000-4000-8000-0000000000b2}", Backend::native, OutputLayout{2, 0x3, 48000}};
        c = OutputTarget{L"{8f4d2a10-0000-4000-8000-0000000000c3}", Backend::equalizer_apo, OutputLayout{8, 0x63F, 48000}};
        outputs = {{a, QStringLiteral("Headphones")}, {b, QStringLiteral("Monitor")}, {c, QStringLiteral("Living room")}};
        session = std::make_unique<EqSession>(link(ns, compat.path()));
        makePresets();
    }
    void makePresets() {
        presets = std::make_unique<Presets>(
            data.path(), session.get(), [this] { return outputs; }, ns, compat.path().toStdWString());
    }
    ~Rig() {
        presets.reset();
        session.reset();
        for (const OutputTarget& t : {a, b, c}) {
            std::filesystem::remove(win::persisted_state_path(win::persisted_state_dir(true), t.guid));
        }
    }
    static std::wstring savedPath(const OutputTarget& t) {
        return win::persisted_state_path(win::persisted_state_dir(true), t.guid);
    }
};

EqState hd650() {
    return peq({band(1, FilterType::LowShelf, 105, 5.5, 0.7, WidthMode::Q),
                band(2, FilterType::Peaking, 180, -2.5, 0.9, WidthMode::Q),
                band(3, FilterType::HighShelf, 10000, -2, 12, WidthMode::SlopeDb)},
               -6.1, false);
}

}  // namespace

TEST_CASE("presets: an unassigned output is untitled and not modified until it is edited") {
    Rig rig("untitled");
    rig.session->useTarget(rig.a);
    CHECK(rig.presets->currentName() == QStringLiteral("Untitled"));
    CHECK(rig.presets->untitled());
    CHECK_FALSE(rig.presets->modified());

    QSignalSpy modified(rig.presets.get(), &Presets::modifiedChanged);
    rig.session->addBand(1000, -3);
    CHECK(rig.presets->modified());
    CHECK(modified.size() == 1);
    // Don't save: back to what it played.
    rig.presets->revert();
    CHECK(rig.session->rowCount() == 0);
    CHECK_FALSE(rig.presets->modified());
    // Save needs a name.
    rig.session->addBand(1000, -3);
    rig.presets->save();
    CHECK(rig.presets->rowCount() == 0);
    CHECK(rig.presets->saveAs(QStringLiteral("Mine")) == QStringLiteral("Mine"));
    CHECK(rig.presets->currentName() == QStringLiteral("Mine"));
    CHECK_FALSE(rig.presets->untitled());
    CHECK_FALSE(rig.presets->modified());
    CHECK(rig.presets->assignedName(QString::fromStdWString(rig.a.guid)) == QStringLiteral("Mine"));
}

TEST_CASE("presets: loading a preset replaces the EQ, keeps what is the output's, and writes its saved state") {
    Rig rig("load");
    isotone::win::SharedMapping engine;
    REQUIRE(engine.create_or_open(isotone::win::mapping_name(rig.ns.c_str(), rig.a.guid)) == ERROR_SUCCESS);

    rig.session->useTarget(rig.a);
    EqState own;
    own.bands = {band(5, FilterType::Notch, 60, 0, 30, WidthMode::Q)};
    own.mute = true;
    own.bypass = true;
    own.speakers.lip_sync_ms = 12;
    rig.session->loadState(&own);
    rig.session->setBalance(-0.5);
    rig.session->finishEdit();
    REQUIRE(rig.presets->saveAs(QStringLiteral("Scratch")) == QStringLiteral("Scratch"));

    {
        // A preset made elsewhere, found on the next start.
        PresetStore store(rig.data.path());
        store.add(QStringLiteral("HD 650"), hd650());
    }
    rig.makePresets();
    REQUIRE(rig.presets->names() == QStringList({QStringLiteral("HD 650"), QStringLiteral("Scratch")}));
    CHECK(rig.presets->currentName() == QStringLiteral("Scratch"));
    std::filesystem::remove(Rig::savedPath(rig.a));

    QSignalSpy current(rig.presets.get(), &Presets::currentChanged);
    rig.presets->load(QStringLiteral("HD 650"));
    CHECK(current.size() >= 1);
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650"));
    CHECK_FALSE(rig.presets->modified());
    REQUIRE(rig.session->rowCount() == 3);
    CHECK(rig.session->bandAt(2)->width_mode == WidthMode::SlopeDb);
    CHECK(rig.session->preampDb() == doctest::Approx(-6.1));
    CHECK_FALSE(rig.session->autoPreamp());
    CHECK(rig.session->muted());
    CHECK_FALSE(rig.session->eqOn());
    CHECK(rig.session->balance() == doctest::Approx(-0.5));
    CHECK(rig.session->state().speakers.lip_sync_ms == 12);

    // What the output plays, and the saved state it starts with, both.
    EqState played = regionState(engine);
    REQUIRE(played.bands.size() == 3);
    CHECK(played.bands[0].fc == doctest::Approx(105));
    CHECK(played.mute);
    CHECK(played.channel_gain_db[1] == doctest::Approx(-6.0206).epsilon(1e-4));
    ParamBlock saved{};
    REQUIRE(win::read_persisted_state(Rig::savedPath(rig.a), &saved) == win::PersistedRead::Loaded);
    EqState file;
    from_param_block(saved, &file);
    REQUIRE(file.bands.size() == 3);
    CHECK(file.preamp_db == doctest::Approx(-6.1));
    CHECK(file.speakers.lip_sync_ms == doctest::Approx(12));

    // One step, undone as one.
    CHECK(rig.session->canUndo());
    rig.session->undo();
    CHECK(rig.session->rowCount() == 1);
    CHECK(rig.presets->modified());
}

TEST_CASE("presets: modified follows committed EQ edits, not balance or mute, and clears on save, load and revert") {
    Rig rig("modified");
    rig.session->useTarget(rig.a);
    {
        PresetStore store(rig.data.path());
        store.add(QStringLiteral("HD 650"), hd650());
        store.add(QStringLiteral("Flat"), peq({}, 0, true));
    }
    rig.makePresets();
    rig.presets->load(QStringLiteral("HD 650"));
    REQUIRE_FALSE(rig.presets->modified());

    rig.session->setBalance(0.4);
    rig.session->finishEdit();
    rig.session->setMuted(true);
    rig.session->setEqOn(false);
    CHECK_FALSE(rig.presets->modified());

    rig.session->setGain(0, 1);   // live, not committed
    CHECK_FALSE(rig.presets->modified());
    rig.session->finishEdit();
    CHECK(rig.presets->modified());

    // Undo back to the saved EQ is not modified.
    rig.session->undo();
    CHECK_FALSE(rig.presets->modified());
    rig.session->redo();
    CHECK(rig.presets->modified());

    rig.session->setAutoPreamp(true);
    rig.presets->save();
    CHECK_FALSE(rig.presets->modified());
    PresetStore disk(rig.data.path());
    REQUIRE(disk.byName(QStringLiteral("HD 650")));
    CHECK(disk.byName(QStringLiteral("HD 650"))->eq.bands[0].gain_db == 1.0);
    CHECK(disk.byName(QStringLiteral("HD 650"))->eq.auto_preamp);

    // Don't save puts the output back to its saved preset.
    rig.session->setFrequency(1, 3000);
    rig.session->deleteBand(2);
    REQUIRE(rig.presets->modified());
    rig.presets->revert();
    CHECK_FALSE(rig.presets->modified());
    REQUIRE(rig.session->rowCount() == 3);
    CHECK(rig.session->bandAt(1)->fc == 180);
    CHECK(rig.session->bandAt(0)->gain_db == 1.0);
    CHECK(rig.session->balance() == doctest::Approx(0.4));   // the output's own
    CHECK(rig.session->muted());

    // With unsaved changes a load asks first, and loads nothing.
    rig.session->setGain(0, -2);
    rig.session->finishEdit();
    QSignalSpy asked(rig.presets.get(), &Presets::unsavedChanges);
    rig.presets->load(QStringLiteral("Flat"));
    REQUIRE(asked.size() == 1);
    CHECK(asked[0][0].toString() == QStringLiteral("Flat"));
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650"));
    CHECK(rig.session->rowCount() == 3);
    rig.presets->next();
    CHECK(asked.size() == 2);
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650"));
}

TEST_CASE("presets: next and previous go round the list in order") {
    Rig rig("next");
    rig.session->useTarget(rig.a);
    {
        PresetStore store(rig.data.path());
        for (const char* n : {"b", "a", "c"}) store.add(QString::fromLatin1(n), peq({}, 0, true));
    }
    rig.makePresets();
    rig.presets->next();   // untitled: the first
    CHECK(rig.presets->currentName() == QStringLiteral("a"));
    rig.presets->next();
    CHECK(rig.presets->currentName() == QStringLiteral("b"));
    rig.presets->previous();
    rig.presets->previous();
    CHECK(rig.presets->currentName() == QStringLiteral("c"));
    rig.presets->next();
    CHECK(rig.presets->currentName() == QStringLiteral("a"));
}

TEST_CASE("presets: the preset an output plays is recognised when the output is shown again") {
    Rig rig("recognise");
    isotone::win::SharedMapping engine;
    REQUIRE(engine.create_or_open(isotone::win::mapping_name(rig.ns.c_str(), rig.a.guid)) == ERROR_SUCCESS);
    rig.session->useTarget(rig.a);
    {
        PresetStore store(rig.data.path());
        EqState eq = hd650();
        eq.bands[1].fc = 180.123456789;   // float32 in the region
        store.add(QStringLiteral("HD 650"), eq);
    }
    rig.makePresets();
    rig.presets->load(QStringLiteral("HD 650"));

    // The app starts again: the output plays the preset from its region.
    rig.presets.reset();
    rig.session = std::make_unique<EqSession>(link(rig.ns, rig.compat.path()));
    rig.makePresets();
    rig.session->useTarget(rig.a);
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650"));
    CHECK_FALSE(rig.presets->modified());
    CHECK_FALSE(rig.session->autoPreamp());
    CHECK(rig.session->bandAt(1)->fc == 180.123456789);   // the preset's values, not float32's

    // Something else changed it meanwhile: modified.
    EqState other = regionState(engine);
    other.bands[0].gain_db = 9;
    param_block_write(engine.params(), [&](ParamBlock* blk) { to_param_block(other, blk); });
    rig.presets.reset();
    rig.session = std::make_unique<EqSession>(link(rig.ns, rig.compat.path()));
    rig.makePresets();
    rig.session->useTarget(rig.a);
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650"));
    CHECK(rig.presets->modified());
}

TEST_CASE("presets: a saved preset reaches every other output assigned it, and unsaved edits do not") {
    Rig rig("propagate");
    isotone::win::SharedMapping engineA, engineB;
    REQUIRE(engineA.create_or_open(isotone::win::mapping_name(rig.ns.c_str(), rig.a.guid)) == ERROR_SUCCESS);
    REQUIRE(engineB.create_or_open(isotone::win::mapping_name(rig.ns.c_str(), rig.b.guid)) == ERROR_SUCCESS);
    // B has a balance of its own.
    EqState bOwn;
    bOwn.channel_gain_db[0] = -3.0103;
    bOwn.layout_channels = 2;
    param_block_write(engineB.params(), [&](ParamBlock* blk) { to_param_block(bOwn, blk); });

    rig.session->useTarget(rig.a);
    {
        PresetStore store(rig.data.path());
        store.add(QStringLiteral("HD 650"), hd650());
    }
    rig.makePresets();
    rig.presets->load(QStringLiteral("HD 650"));
    rig.presets->assign(QString::fromStdWString(rig.b.guid), QStringLiteral("HD 650"));
    rig.presets->assign(QString::fromStdWString(rig.c.guid), QStringLiteral("HD 650"));
    CHECK(rig.presets->assignedName(QString::fromStdWString(rig.c.guid)) == QStringLiteral("HD 650"));
    REQUIRE(regionState(engineB).bands.size() == 3);
    CHECK(regionState(engineB).channel_gain_db[0] == doctest::Approx(-3.0103));
    // A preset is for every output until it is narrowed (owner, 2026-09-15).
    CHECK(rig.presets->data(rig.presets->index(0), Presets::AssignedRole).toString().isEmpty());

    // An unsaved edit stays on A.
    rig.session->setGain(0, -9);
    rig.session->finishEdit();
    CHECK(regionState(engineA).bands[0].gain_db == doctest::Approx(-9));
    CHECK(regionState(engineB).bands[0].gain_db == doctest::Approx(5.5));

    // Saved, it reaches B (region and saved state, its balance kept) and C (Isotone.txt, for 7.1).
    rig.presets->save();
    EqState onB = regionState(engineB);
    REQUIRE(onB.bands.size() == 3);
    CHECK(onB.bands[0].gain_db == doctest::Approx(-9));
    CHECK(onB.channel_gain_db[0] == doctest::Approx(-3.0103));
    ParamBlock saved{};
    REQUIRE(win::read_persisted_state(Rig::savedPath(rig.b), &saved) == win::PersistedRead::Loaded);
    EqState fileB;
    from_param_block(saved, &fileB);
    CHECK(fileB.bands[0].gain_db == doctest::Approx(-9));

    ui::DeviceLink reader(rig.ns, rig.compat.path().toStdWString());
    reader.set_target(rig.c);
    EqState onC;
    REQUIRE(reader.load_current(&onC));
    REQUIRE(onC.bands.size() == 3);
    CHECK(onC.bands[0].gain_db == doctest::Approx(-9));
    CHECK(onC.layout_channels == 8);
    CHECK(onC.preamp_db == doctest::Approx(-6.1));
}

TEST_CASE("presets: rename, duplicate, remove and New on the current output") {
    Rig rig("manage");
    rig.session->useTarget(rig.a);
    {
        PresetStore store(rig.data.path());
        store.add(QStringLiteral("HD 650"), hd650());
    }
    rig.makePresets();
    rig.presets->load(QStringLiteral("HD 650"));

    CHECK(rig.presets->rename(QStringLiteral("HD 650"), QStringLiteral("HD 650 · tuned")) == QStringLiteral("HD 650 · tuned"));
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650 · tuned"));
    CHECK(rig.presets->assignedName(QString::fromStdWString(rig.a.guid)) == QStringLiteral("HD 650 · tuned"));

    CHECK(rig.presets->duplicate(QStringLiteral("HD 650 · tuned")) == QStringLiteral("HD 650 · tuned 2"));
    CHECK(rig.presets->rowCount() == 2);
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650 · tuned"));   // the copy is not loaded
    CHECK(rig.presets->data(rig.presets->index(1), Presets::AssignedRole).toString().isEmpty());
    CHECK(rig.presets->data(rig.presets->index(0), Presets::CurrentRole).toBool());

    rig.presets->newPreset(true);
    CHECK(rig.presets->currentName() == QStringLiteral("Untitled"));
    CHECK(rig.presets->modified());
    CHECK(rig.session->rowCount() == 0);
    CHECK(rig.session->autoPreamp());
    CHECK(rig.session->preampDb() == 0.0);
    // Don't save after New: back to the preset it had.
    rig.presets->revert();
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650 · tuned"));
    CHECK(rig.session->rowCount() == 3);
    CHECK_FALSE(rig.presets->modified());

    rig.presets->newPreset(false);
    CHECK_FALSE(rig.session->autoPreamp());
    rig.session->addBand(500, 2);
    CHECK(rig.presets->saveAs(QStringLiteral("HD 650 · tuned")) == QStringLiteral("HD 650 · tuned 3"));
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650 · tuned 3"));

    // Removing the current output's preset leaves it untitled, playing what it plays.
    rig.presets->remove(QStringLiteral("HD 650 · tuned 3"));
    CHECK(rig.presets->rowCount() == 2);
    CHECK(rig.presets->currentName() == QStringLiteral("Untitled"));
    CHECK_FALSE(rig.presets->modified());
    CHECK(rig.session->rowCount() == 1);
}

TEST_CASE("presets: import reads for the output it is for, creates the preset and loads it") {
    Rig rig("import");
    rig.session->useTarget(rig.a);
    rig.makePresets();
    const QString file = QDir(rig.data.path()).filePath(QStringLiteral("Sennheiser HD 650 ParametricEQ.txt"));
    writeFile(file, "Preamp: -6.1 dB\n"
                    "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\n"
                    "Filter 2: ON PK Fc 180 Hz Gain -2.5 dB Q 0.90\n"
                    "Channel: R\n"
                    "Filter 3: ON PK Fc 3100 Hz Gain 2.5 dB Q 3\n"
                    "Include: other.txt\n"
                    "GraphicEQ: 20 -2.1; 21 -2.0\n");
    std::unique_ptr<ImportPreview> preview(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(preview);
    CHECK(preview->fileName() == QStringLiteral("Sennheiser HD 650 ParametricEQ.txt"));
    CHECK(preview->suggestedName() == QStringLiteral("Sennheiser HD 650 ParametricEQ"));
    // For every output unless one is picked (owner, 2026-09-15): it is still read
    // for the current output's layout.
    CHECK(preview->forEveryOutput());
    CHECK(preview->outputGuid().isEmpty());
    CHECK(preview->filterCount() == 3);
    CHECK(preview->preampDb() == doctest::Approx(-6.1));
    CHECK(preview->usable());
    const QVariantList skipped = preview->skipped();
    REQUIRE(skipped.size() == 2);
    CHECK(skipped[0].toMap()[QStringLiteral("line")].toInt() == 6);
    CHECK(skipped[0].toMap()[QStringLiteral("text")].toString() == QStringLiteral("Include: other.txt"));
    CHECK(skipped[1].toMap()[QStringLiteral("line")].toInt() == 7);
    CHECK(preview->state().bands[2].channels == 0x2);

    // For the 7.1 output, "R" is its front right: still channel 1; the layout follows.
    QSignalSpy changed(preview.get(), &ImportPreview::changed);
    preview->setOutputGuid(QString::fromStdWString(rig.c.guid));
    CHECK(changed.size() == 1);
    CHECK(preview->state().layout_channels == 8);

    // Imported for C, which is not the current output: written there, A untouched.
    CHECK(rig.presets->importPreset(preview.get(), QStringLiteral("HD 650 · oratory1990")) ==
          QStringLiteral("HD 650 · oratory1990"));
    CHECK(rig.presets->assignedName(QString::fromStdWString(rig.c.guid)) == QStringLiteral("HD 650 · oratory1990"));
    CHECK(rig.session->rowCount() == 0);
    CHECK(rig.presets->currentName() == QStringLiteral("Untitled"));

    // For A: loaded on it.
    preview->setOutputGuid(QString::fromStdWString(rig.a.guid));
    CHECK(rig.presets->importPreset(preview.get(), QStringLiteral("HD 650 · oratory1990")) ==
          QStringLiteral("HD 650 · oratory1990 2"));
    CHECK(rig.presets->currentName() == QStringLiteral("HD 650 · oratory1990 2"));
    CHECK(rig.session->rowCount() == 3);
    CHECK_FALSE(rig.presets->modified());

    // For every output: created, loaded here, and no other output written.
    preview->setOutputGuid(QString());
    CHECK(preview->forEveryOutput());
    CHECK(rig.presets->importPreset(preview.get(), QStringLiteral("For everything")) ==
          QStringLiteral("For everything"));
    CHECK(rig.presets->currentName() == QStringLiteral("For everything"));
    CHECK(rig.presets->assignedName(QString::fromStdWString(rig.c.guid)) == QStringLiteral("HD 650 · oratory1990"));

    // A file with no preamp of its own has none chosen, so Auto (owner, 2026-09-15).
    writeFile(file, "Filter 1: ON PK Fc 1000 Hz Gain 8 dB Q 1\n");
    std::unique_ptr<ImportPreview> noPreamp(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(noPreamp);
    CHECK(noPreamp->preampDb() == 0.0);
    CHECK(rig.presets->importPreset(noPreamp.get(), QStringLiteral("No preamp")) == QStringLiteral("No preamp"));
    CHECK(rig.session->autoPreamp());
    CHECK(rig.session->preampDb() < -7.0);   // Auto made room for the 8 dB bell

    // A file that carries one keeps it, by hand.
    writeFile(file, "Preamp: -1 dB\nFilter 1: ON PK Fc 1000 Hz Gain 8 dB Q 1\n");
    std::unique_ptr<ImportPreview> withPreamp(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(withPreamp);
    CHECK(rig.presets->importPreset(withPreamp.get(), QStringLiteral("Its own preamp")) ==
          QStringLiteral("Its own preamp"));
    CHECK_FALSE(rig.session->autoPreamp());
    CHECK(rig.session->preampDb() == doctest::Approx(-1.0));

    // A curve, not filters: bands are fitted to it (the owner's export, 2026-09-15).
    writeFile(file, "GraphicEQ: 20 -6; 200 6; 2000 -3; 20000 2\n");
    std::unique_ptr<ImportPreview> curve(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(curve);
    CHECK(curve->usable());
    CHECK(curve->filterCount() > 0);
    CHECK(curve->curvePoints() == 4);
    CHECK(curve->fitWorstDb() < 2.0);
    CHECK(curve->skipped().isEmpty());   // nothing was skipped: it was read

    // Nothing usable: a line the importer understands and cannot play.
    writeFile(file, "Convolution: room.wav\n");
    std::unique_ptr<ImportPreview> empty(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(empty);
    CHECK_FALSE(empty->usable());
    CHECK(empty->filterCount() == 0);
    CHECK(empty->curvePoints() == 0);
    CHECK(empty->skipped().size() == 1);
    CHECK(rig.presets->importPreset(empty.get(), QStringLiteral("Nothing")).isEmpty());
    CHECK(rig.presets->rowCount() == 5);   // nothing was added for the unusable file

    CHECK(rig.presets->openImport(QUrl::fromLocalFile(file + QStringLiteral(".missing"))) == nullptr);

    // Notepad's byte order marks: the first line is still a Preamp line.
    writeFile(file, "\xEF\xBB\xBFPreamp: -2 dB\r\nFilter 1: ON PK Fc 100 Hz Gain -3 dB Q 1\r\n");
    std::unique_ptr<ImportPreview> utf8(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(utf8);
    CHECK(utf8->preampDb() == doctest::Approx(-2));
    CHECK(utf8->skipped().isEmpty());
    const QString wide = QStringLiteral("Preamp: -2 dB\r\nFilter 1: ON PK Fc 100 Hz Gain -3 dB Q 1\r\n");
    QByteArray utf16("\xFF\xFE", 2);
    utf16.append(reinterpret_cast<const char*>(wide.utf16()), wide.size() * 2);
    writeFile(file, utf16);
    std::unique_ptr<ImportPreview> unicode(rig.presets->openImport(QUrl::fromLocalFile(file)));
    REQUIRE(unicode);
    CHECK(unicode->preampDb() == doctest::Approx(-2));
    CHECK(unicode->filterCount() == 1);
}

TEST_CASE("presets: export writes the output's EQ for a layout") {
    Rig rig("export");
    rig.session->useTarget(rig.c);   // 7.1
    EqState eq = hd650();
    eq.layout_channels = 8;
    eq.layout_speaker_mask = 0x63F;
    eq.bands[1].channels = 0x40;   // SL on 7.1
    rig.session->setEqPart(eq);

    const QVariantList layouts = rig.presets->exportLayouts();
    REQUIRE(layouts.size() == 3);
    CHECK(layouts[0].toMap()[QStringLiteral("label")].toString() == QStringLiteral("7.1"));
    CHECK(layouts[1].toMap()[QStringLiteral("label")].toString() == QStringLiteral("5.1"));
    CHECK(layouts[2].toMap()[QStringLiteral("label")].toString() == QStringLiteral("Stereo"));

    const QString text = rig.presets->exportText(6, 0x60F);
    EqState forFive = rig.session->eqPart();
    ApoFormatOptions options;
    options.layout = ChannelLayout{6, 0x60F};
    CHECK(text == QString::fromStdString(format_apo_config(forFive, options)));
    CHECK(text.contains(QStringLiteral("Channel: SL")));
    CHECK(text.contains(QStringLiteral("Preamp: -6.1 dB")));

    const QString out = QDir(rig.data.path()).filePath(QStringLiteral("out.txt"));
    CHECK(rig.presets->exportFile(QUrl::fromLocalFile(out), 8, 0x63F));
    CHECK(readFile(out) == rig.presets->exportText(8, 0x63F).toUtf8());

    rig.session->useTarget(rig.a);
    CHECK(rig.presets->exportLayouts().isEmpty());
}

TEST_CASE("presets: a preset is for every output until it is narrowed to one") {
    // The owner asked to be asked on Save as, and to be able to change it after
    // (2026-09-15). Before this a preset was tied to the output it was saved on.
    Rig rig("scope");
    rig.session->useTarget(rig.a);
    const QString a = QString::fromStdWString(rig.a.guid);
    const QString b = QString::fromStdWString(rig.b.guid);

    rig.session->setEqPart(hd650());
    CHECK(rig.presets->saveAs(QStringLiteral("Everywhere")) == QStringLiteral("Everywhere"));
    CHECK(rig.presets->presetOutput(QStringLiteral("Everywhere")).isEmpty());

    rig.session->setEqPart(hd650());
    CHECK(rig.presets->saveAs(QStringLiteral("Just A"), a) == QStringLiteral("Just A"));
    CHECK(rig.presets->presetOutput(QStringLiteral("Just A")) == a);

    // Saved for another output, from this one: still listed here, and still this
    // output's preset. Hiding it made it vanish as soon as it was saved (owner).
    rig.session->setEqPart(hd650());
    CHECK(rig.presets->saveAs(QStringLiteral("Just B"), b) == QStringLiteral("Just B"));
    CHECK(rig.presets->currentName() == QStringLiteral("Just B"));
    CHECK(rig.presets->names() ==
          QStringList({QStringLiteral("Everywhere"), QStringLiteral("Just A"), QStringLiteral("Just B")}));

    // Every output lists every preset; the row says what each is for.
    rig.session->useTarget(rig.b);
    CHECK(rig.presets->names() ==
          QStringList({QStringLiteral("Everywhere"), QStringLiteral("Just A"), QStringLiteral("Just B")}));
    CHECK(rig.presets->data(rig.presets->index(0), Presets::AssignedRole).toString().isEmpty());
    CHECK(rig.presets->data(rig.presets->index(1), Presets::AssignedRole).toString() == QStringLiteral("Headphones"));
    CHECK(rig.presets->data(rig.presets->index(2), Presets::ForOutputRole).toString() == b);

    // Narrowed and widened again, from either output.
    rig.presets->setPresetOutput(QStringLiteral("Everywhere"), b);
    CHECK(rig.presets->presetOutput(QStringLiteral("Everywhere")) == b);
    CHECK(rig.presets->data(rig.presets->index(0), Presets::AssignedRole).toString() == QStringLiteral("Monitor"));
    rig.session->useTarget(rig.a);
    CHECK(rig.presets->names().size() == 3);
    rig.presets->setPresetOutput(QStringLiteral("Everywhere"), QString());
    CHECK(rig.presets->presetOutput(QStringLiteral("Everywhere")).isEmpty());

    // It survives a restart: the file carries it.
    rig.makePresets();
    CHECK(rig.presets->presetOutput(QStringLiteral("Just A")) == a);
    CHECK(rig.presets->presetOutput(QStringLiteral("Everywhere")).isEmpty());
}
