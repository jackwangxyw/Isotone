// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "presets.h"

#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSaveFile>
#include <QStringDecoder>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

#include "apppaths.h"
#include "eqsession.h"
#include "importpreview.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "isotone/response.h"
#include "output_state.h"
#include "outputs.h"
#include "speaker_layout.h"

namespace {

const QString kUntitled = QStringLiteral("Untitled");

// A loaded preamp this close to Auto's value is Auto's (as EqSession reads an output).
constexpr double kAutoPreampMatchDb = 0.01;

double auto_value(const isotone::EqState& state, const isotone::ui::OutputLayout& layout) {
    static const std::vector<double> grid = isotone::log_grid(20.0, 20000.0, 512);
    isotone::EqState probe = state;
    probe.bypass = false;   // as the bands play with EQ on, as EqSession computes it
    isotone::ui::clear_balance(layout.channels, &probe);
    return isotone::auto_preamp_db(probe, layout.channels, layout.speaker_mask, grid.data(), grid.size(),
                                   layout.sample_rate > 0 ? layout.sample_rate : 48000.0);
}

// Text from a file as UTF-8: without a UTF-8 byte order mark, and UTF-16 converted.
std::string decode(const QByteArray& bytes) {
    if (bytes.startsWith("\xEF\xBB\xBF")) return bytes.mid(3).toStdString();
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        QStringDecoder decoder(bytes.startsWith("\xFF\xFE") ? QStringConverter::Utf16LE : QStringConverter::Utf16BE);
        return QString(decoder.decode(bytes.mid(2))).toStdString();
    }
    return bytes.toStdString();
}

}  // namespace

Presets* Presets::create(QQmlEngine* qml, QJSEngine*) {
    auto* session = qml->singletonInstance<EqSession*>("Isotone", "EqSession");
    // Outputs is asked when needed: it enumerates the machine's endpoints.
    OutputList outputs = [qml] {
        std::vector<OutputInfo> list;
        auto* o = qml->singletonInstance<Outputs*>("Isotone", "Outputs");
        if (!o) return list;
        for (const Outputs::Output& out : o->outputs())
            list.push_back(OutputInfo{isotone::ui::OutputTarget{out.guid, out.backend, out.layout}, out.name});
        return list;
    };
    return new Presets(AppPaths::dataDir(), session, std::move(outputs), L"Global\\",
                       AppPaths::compatConfigDir().toStdWString());
}

Presets::Presets(const QString& data_dir, EqSession* session, OutputList outputs, std::wstring region_namespace,
                 std::wstring compat_dir, QObject* parent)
    : QAbstractListModel(parent),
      store_(data_dir),
      session_(session),
      outputs_(std::move(outputs)),
      namespace_(std::move(region_namespace)),
      compat_dir_(std::move(compat_dir)) {
    if (session_) {
        connect(session_, &EqSession::targetChanged, this, &Presets::outputChanged);
        connect(session_, &EqSession::committed, this, &Presets::refresh);
    }
    outputChanged();
}

Presets::~Presets() = default;

// ---------------------------------------------------------------------------
// The model

int Presets::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(store_.presets().size());
}

// Every preset is listed on every output, whichever one it is for: hiding the
// rest made a preset saved for another output vanish as soon as it was saved
// (owner, 2026-09-15). What it is for is on its row, and it is what an output
// picks up when it becomes the default one.
const PresetStore::Preset* Presets::presetAt(int row) const {
    if (row < 0 || row >= rowCount()) return nullptr;
    return &store_.presets()[static_cast<size_t>(row)];
}

// A preset is for every output, or for one: narrowed to another output it is
// listed here but does not load (owner, 2026-09-16). A GUID may be spelled in
// either case, depending on where it was read.
bool Presets::loadableHere(const PresetStore::Preset& p) const {
    return p.for_output.isEmpty() || p.for_output.compare(currentGuid(), Qt::CaseInsensitive) == 0;
}

QVariant Presets::data(const QModelIndex& index, int role) const {
    const PresetStore::Preset* at = presetAt(index.row());
    if (at == nullptr) return {};
    const PresetStore::Preset& p = *at;
    switch (role) {
        case NameRole: return p.name;
        case CurrentRole: {
            const PresetStore::Preset* current = currentPreset();
            return current && current->id == p.id && !untitled();
        }
        // What the preset is for: an output's name, or empty for every output.
        case AssignedRole: return p.for_output.isEmpty() ? QString() : outputName(p.for_output);
        case ForOutputRole: return p.for_output;
        case LoadableRole: return loadableHere(p);
    }
    return {};
}

QHash<int, QByteArray> Presets::roleNames() const {
    return {{NameRole, "name"},
            {AssignedRole, "assigned"},
            {CurrentRole, "current"},
            {ForOutputRole, "forOutput"},
            {LoadableRole, "loadable"}};
}

QStringList Presets::names() const {
    QStringList list;
    for (const PresetStore::Preset& p : store_.presets()) list << p.name;
    return list;
}

void Presets::rebuild() {
    beginResetModel();
    endResetModel();
    emit countChanged();
}

// ---------------------------------------------------------------------------
// The current output

QString Presets::currentGuid() const { return session_ ? QString::fromStdWString(session_->target().guid) : QString(); }

Presets::OutputMemory& Presets::memory() { return memory_[currentGuid()]; }

const PresetStore::Preset* Presets::currentPreset() const {
    const QString guid = currentGuid();
    return store_.byId(guid.isEmpty() ? none_assignment_ : store_.assignment(guid));
}

bool Presets::untitled() const {
    const auto it = memory_.find(currentGuid());
    return currentPreset() == nullptr || (it != memory_.end() && it->second.detached);
}

QString Presets::currentName() const { return current_name_; }

QString Presets::outputName(const QString& guid) const {
    if (outputs_) {
        for (const OutputInfo& o : outputs_())
            if (QString::fromStdWString(o.target.guid).compare(guid, Qt::CaseInsensitive) == 0) return o.name;
    }
    return store_.outputName(guid);
}

void Presets::setAssignment(const QString& guid, const QString& id) {
    if (guid.isEmpty()) {
        none_assignment_ = id;
    } else {
        store_.assign(guid, id, outputName(guid));
    }
}

void Presets::outputChanged() {
    if (!session_) return;
    const QString guid = currentGuid();
    OutputMemory& m = memory();
    // An output does not take up a preset that is for another one. It can be
    // loaded there on purpose and stays while it is loaded, but coming back to
    // this output does not bring it back (owner, 2026-09-16).
    if (const PresetStore::Preset* had = currentPreset()) {
        // A GUID may be spelled in either case, depending on where it was read.
        if (!had->for_output.isEmpty() && had->for_output.compare(guid, Qt::CaseInsensitive) != 0) {
            setAssignment(guid, QString());
            m.detached = false;
        }
    }
    const PresetStore::Preset* p = currentPreset();
    if (p && !m.detached) {
        // What the output plays is its preset: take the preset's exact values,
        // ids and Auto mode (the output keeps float32 and no mode). A preset
        // with a preamp set by hand may have been read as Auto.
        isotone::EqState playing = session_->eqPart();
        if (!p->eq.auto_preamp) playing.auto_preamp = false;
        if (same_eq(playing, p->eq)) session_->adoptEqPart(p->eq);
        if (!guid.isEmpty()) store_.setOutputName(guid, outputName(guid));
    }
    if (!p && !m.has_baseline) {
        m.baseline = session_->eqPart();
        m.has_baseline = true;
    }
    refresh();
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), {AssignedRole, CurrentRole, LoadableRole});
}

void Presets::refresh() {
    if (!session_) return;
    OutputMemory& m = memory();
    const PresetStore::Preset* p = currentPreset();
    QString name;
    bool modified = false;
    if (m.detached || !p) {
        name = kUntitled;
        if (m.detached) {
            modified = true;
        } else {
            if (!m.has_baseline) {
                m.baseline = session_->eqPart();
                m.has_baseline = true;
            }
            modified = !same_eq(session_->eqPart(), m.baseline);
        }
    } else {
        name = p->name;
        modified = !same_eq(session_->eqPart(), p->eq);
    }
    const bool name_changed = name != current_name_;
    current_name_ = name;
    if (name_changed) emit currentChanged();
    if (modified != modified_) {
        modified_ = modified;
        emit modifiedChanged();
    }
}

void Presets::apply(const PresetStore::Preset& preset) {
    const PresetStore::Preset p = preset;   // the store may move it
    memory().detached = false;
    setAssignment(currentGuid(), p.id);
    session_->setEqPart(p.eq);
    session_->saveToOutput();
    refresh();
    emit currentChanged();
    if (rowCount() > 0)
        emit dataChanged(index(0), index(rowCount() - 1), {AssignedRole, CurrentRole, LoadableRole});
}

void Presets::writeTo(const isotone::ui::OutputTarget& target, const isotone::EqState& eq) {
    isotone::ui::DeviceLink link(namespace_, compat_dir_);
    link.set_target(target);
    const isotone::ChannelLayout layout{target.layout.channels, target.layout.speaker_mask};
    isotone::EqState state;
    if (link.load_current(&state)) isotone::remap_channels(&state, layout);
    isotone::EqState moved = eq;
    // The region holds kParamMaxBands: an IsoAPO output gets the first ones, as EqSession loads them.
    if (target.backend == isotone::ui::Backend::native && moved.bands.size() > isotone::kParamMaxBands)
        moved.bands.resize(isotone::kParamMaxBands);
    isotone::remap_channels(&moved, layout);
    state.bands = std::move(moved.bands);
    state.auto_preamp = eq.auto_preamp;
    state.preamp_db = eq.preamp_db;
    state.layout_channels = target.layout.channels;
    state.layout_speaker_mask = target.layout.speaker_mask;
    if (state.auto_preamp) state.preamp_db = auto_value(state, target.layout);
    link.commit(state);
    if (target.backend == isotone::ui::Backend::native) link.save(state, state);
}

void Presets::propagate(const QString& id) {
    const PresetStore::Preset* p = store_.byId(id);
    if (!p || !outputs_) return;
    const isotone::EqState eq = p->eq;
    const QString current = currentGuid();
    for (const OutputInfo& o : outputs_()) {
        const QString guid = QString::fromStdWString(o.target.guid);
        if (guid == current || store_.assignment(guid) != id) continue;
        memory_.erase(guid);
        writeTo(o.target, eq);
    }
}

// ---------------------------------------------------------------------------
// Actions

void Presets::load(const QString& name) {
    const PresetStore::Preset* p = store_.byName(name);
    if (!p || !session_ || !loadableHere(*p)) return;
    if (modified_) {
        emit unsavedChanges(name);
        return;
    }
    apply(*p);
}

void Presets::step(int by) {
    const QStringList list = names();
    if (list.isEmpty()) return;
    const int at = untitled() ? -1 : static_cast<int>(list.indexOf(current_name_));
    const int n = static_cast<int>(list.size());
    const int to = by > 0 ? (at + 1) % n : (at <= 0 ? n - 1 : at - 1);
    load(list[to]);
}

void Presets::next() { step(1); }
void Presets::previous() { step(-1); }

void Presets::save() {
    if (!session_ || untitled()) return;
    const QString id = currentPreset()->id;
    if (!store_.update(id, session_->eqPart())) return;
    session_->saveToOutput();
    propagate(id);
    refresh();
}

QString Presets::saveAs(const QString& name, const QString& forOutput) {
    if (!session_) return QString();
    const QString id = store_.add(name, session_->eqPart(), forOutput);
    if (id.isEmpty()) return QString();
    // Saved for another output: it is made, and this one is left as it was, since
    // a preset for another output does not load here.
    if (!loadableHere(*store_.byId(id))) {
        rebuild();
        refresh();
        return store_.byId(id)->name;
    }
    memory().detached = false;
    setAssignment(currentGuid(), id);
    session_->saveToOutput();
    rebuild();
    refresh();
    emit currentChanged();
    return store_.byId(id)->name;
}

void Presets::setPresetOutput(const QString& name, const QString& guid) {
    const PresetStore::Preset* p = store_.byName(name);
    if (!p || p->for_output == guid) return;
    if (!store_.setForOutput(p->id, guid)) return;
    rebuild();
    refresh();
    emit currentChanged();
}

QString Presets::presetOutput(const QString& name) const {
    const PresetStore::Preset* p = store_.byName(name);
    return p ? p->for_output : QString();
}

QString Presets::assignedName(const QString& guid) const {
    const PresetStore::Preset* p = store_.byId(guid.isEmpty() ? none_assignment_ : store_.assignment(guid));
    return p ? p->name : QString();
}

void Presets::assign(const QString& guid, const QString& name) {
    const PresetStore::Preset* p = name.isEmpty() ? nullptr : store_.byName(name);
    if (!name.isEmpty() && !p) return;
    if (session_ && guid == currentGuid()) {
        if (p) {
            load(name);
            return;
        }
        // No preset: untitled, playing what it plays.
        setAssignment(guid, QString());
        OutputMemory& m = memory();
        m.detached = false;
        m.baseline = session_->eqPart();
        m.has_baseline = true;
        refresh();
        emit currentChanged();
        return;
    }
    setAssignment(guid, p ? p->id : QString());
    memory_.erase(guid);
    if (p && outputs_) {
        for (const OutputInfo& o : outputs_())
            if (QString::fromStdWString(o.target.guid) == guid) writeTo(o.target, p->eq);
    }
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {AssignedRole, LoadableRole});
}

QString Presets::rename(const QString& name, const QString& to) {
    const PresetStore::Preset* p = store_.byName(name);
    if (!p) return QString();
    const QString renamed = store_.rename(p->id, to);
    if (renamed.isEmpty()) return QString();
    rebuild();
    refresh();
    return renamed;
}

QString Presets::duplicate(const QString& name) {
    const PresetStore::Preset* p = store_.byName(name);
    if (!p) return QString();
    const isotone::EqState eq = p->eq;
    const QString id = store_.add(name, eq);
    if (id.isEmpty()) return QString();
    rebuild();
    return store_.byId(id)->name;
}

void Presets::remove(const QString& name) {
    const PresetStore::Preset* p = store_.byName(name);
    if (!p) return;
    const QString id = p->id;
    const std::vector<QString> guids = store_.assignedTo(id);
    if (!store_.remove(id)) return;
    // Its outputs start untitled: what they play when next shown is their baseline.
    for (const QString& guid : guids) memory_.erase(guid);
    if (none_assignment_ == id) {
        none_assignment_.clear();
        memory_.erase(QString());
    }
    rebuild();
    refresh();
}

void Presets::newPreset(bool autoPreamp) {
    if (!session_) return;
    memory().detached = true;
    isotone::EqState flat;
    flat.auto_preamp = autoPreamp;
    session_->setEqPart(flat);
    refresh();
    emit currentChanged();
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {CurrentRole});
}

void Presets::revert() {
    if (!session_) return;
    OutputMemory& m = memory();
    m.detached = false;
    if (const PresetStore::Preset* p = currentPreset()) {
        const isotone::EqState eq = p->eq;
        session_->setEqPart(eq);
    } else if (m.has_baseline) {
        const isotone::EqState eq = m.baseline;
        session_->setEqPart(eq);
    }
    refresh();
    emit currentChanged();
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {CurrentRole});
}

QString Presets::uniqueName(const QString& base) const { return store_.uniqueName(base); }

// ---------------------------------------------------------------------------
// Import and export

ImportPreview* Presets::openImport(const QUrl& file) {
    QFile f(file.toLocalFile());
    if (file.toLocalFile().isEmpty() || !f.open(QIODevice::ReadOnly)) return nullptr;
    std::vector<isotone::ui::OutputTarget> targets;
    if (outputs_) {
        for (const OutputInfo& o : outputs_()) targets.push_back(o.target);
    }
    if (session_ && session_->target().backend == isotone::ui::Backend::none) targets.push_back(session_->target());
    return new ImportPreview(QFileInfo(f).fileName(), decode(f.readAll()), std::move(targets),
                             session_ ? session_->target().guid : std::wstring());
}

QString Presets::importPreset(ImportPreview* preview, const QString& name) {
    if (!preview || !preview->usable() || !session_) return QString();
    const isotone::ui::OutputTarget target = preview->target();
    isotone::EqState eq = eq_part(preview->state(), preview->state().layout_channels, preview->state().layout_speaker_mask);
    // A file carries no Auto mode. No preamp in it at all (a curve, or a config
    // without a Preamp line) means none was chosen, so Auto (owner, 2026-09-15);
    // otherwise Auto when its preamp is what Auto would set.
    eq.auto_preamp =
        eq.preamp_db == 0.0 || std::abs(auto_value(eq, target.layout) - eq.preamp_db) <= kAutoPreampMatchDb;
    const QString id = store_.add(name, eq);
    if (id.isEmpty()) return QString();
    // For every output: loaded here, and no other output written.
    const QString guid = preview->forEveryOutput() ? currentGuid() : QString::fromStdWString(target.guid);
    rebuild();
    if (guid == currentGuid()) {
        apply(*store_.byId(id));
    } else {
        setAssignment(guid, id);
        memory_.erase(guid);
        writeTo(target, eq);
    }
    return store_.byId(id)->name;
}

QVariantList Presets::outputChoices() const {
    QVariantList list;
    if (!outputs_) return list;
    const QString current = currentGuid();
    for (const OutputInfo& o : outputs_()) {
        const QString guid = QString::fromStdWString(o.target.guid);
        const QVariantMap entry{
            {QStringLiteral("guid"), guid}, {QStringLiteral("name"), o.name}, {QStringLiteral("current"), guid == current}};
        if (guid == current) {
            list.prepend(entry);
        } else {
            list.append(entry);
        }
    }
    return list;
}

QVariantList Presets::exportLayouts() const {
    QVariantList list;
    if (!session_) return list;
    const isotone::ui::OutputLayout& own = session_->target().layout;
    if (session_->target().backend == isotone::ui::Backend::none || own.channels <= 2) return list;
    const auto entry = [](const QString& label, uint32_t channels, uint32_t mask) {
        return QVariantMap{{QStringLiteral("label"), label},
                           {QStringLiteral("channels"), static_cast<int>(channels)},
                           {QStringLiteral("mask"), static_cast<int>(mask)}};
    };
    QString label = QStringLiteral("%1 ch").arg(own.channels);
    for (const isotone::devices::SpeakerLayoutSpec& spec : isotone::devices::kSpeakerLayouts) {
        if (spec.channels == own.channels && spec.mask == own.speaker_mask) label = QString::fromLatin1(spec.name);
    }
    list << entry(label, own.channels, own.speaker_mask);
    // As the prototype offers them: the output's own, then 5.1 and stereo below it.
    const auto& five = isotone::devices::kSpeakerLayouts[2];
    if (own.channels > five.channels) list << entry(QStringLiteral("5.1"), five.channels, five.mask);
    list << entry(QStringLiteral("Stereo"), 2, 0x3);
    return list;
}

QString Presets::exportText(int channels, int speakerMask) const {
    if (!session_) return QString();
    isotone::ApoFormatOptions options;
    if (channels > 0) options.layout = isotone::ChannelLayout{static_cast<uint32_t>(channels), static_cast<uint32_t>(speakerMask)};
    return QString::fromStdString(isotone::format_apo_config(session_->eqPart(), options));
}

bool Presets::exportFile(const QUrl& file, int channels, int speakerMask) const {
    if (file.toLocalFile().isEmpty()) return false;
    QSaveFile f(file.toLocalFile());
    const QByteArray bytes = exportText(channels, speakerMask).toUtf8();
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
