// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "presetstore.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>

#include "isotone/apo_config.h"

namespace {

const char* const kTypeNames[] = {"peaking", "lowPass", "highPass", "bandPass", "notch", "allPass", "lowShelf", "highShelf"};
const char* const kWidthNames[] = {"q", "bandwidthOct", "slopeDb"};

// An integer a double holds exactly, in [0, max].
bool whole(const QJsonValue& v, double max, double* out) {
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < 0 || d > max || std::floor(d) != d) return false;
    *out = d;
    return true;
}

bool finite(const QJsonValue& v, double* out) {
    if (!v.isDouble() || !std::isfinite(v.toDouble())) return false;
    *out = v.toDouble();
    return true;
}

int indexOf(const QJsonValue& v, const char* const* names, int n) {
    if (!v.isString()) return -1;
    for (int i = 0; i < n; ++i)
        if (v.toString() == QLatin1String(names[i])) return i;
    return -1;
}

bool writeAtomically(const QString& path, const QByteArray& bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

bool near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

}  // namespace

namespace presetfile {

QByteArray write(const QString& name, const isotone::EqState& eq, const QString& for_output) {
    QJsonArray bands;
    for (const isotone::Band& b : eq.bands) {
        bands.append(QJsonObject{
            {QStringLiteral("id"), static_cast<double>(b.id)},
            {QStringLiteral("type"), QLatin1String(kTypeNames[static_cast<int>(b.type)])},
            {QStringLiteral("fc"), b.fc},
            {QStringLiteral("gainDb"), b.gain_db},
            {QStringLiteral("width"), b.width},
            {QStringLiteral("widthMode"), QLatin1String(kWidthNames[static_cast<int>(b.width_mode)])},
            {QStringLiteral("shelfCorner"), b.shelf_corner},
            {QStringLiteral("channels"), static_cast<double>(b.channels)},
            {QStringLiteral("enabled"), b.enabled},
        });
    }
    QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("isotone-preset")},
        {QStringLiteral("version"), kVersion},
        {QStringLiteral("name"), name},
        {QStringLiteral("preampDb"), eq.preamp_db},
        {QStringLiteral("autoPreamp"), eq.auto_preamp},
        {QStringLiteral("layout"),
         QJsonObject{{QStringLiteral("channels"), static_cast<double>(eq.layout_channels)},
                     {QStringLiteral("speakerMask"), static_cast<double>(eq.layout_speaker_mask)}}},
        {QStringLiteral("bands"), bands},
    };
    if (!for_output.isEmpty()) root.insert(QStringLiteral("forOutput"), for_output);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool read(const QByteArray& bytes, QString* name, isotone::EqState* eq, QString* for_output) {
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject root = doc.object();
    double version = 0;
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("isotone-preset") ||
        !whole(root.value(QStringLiteral("version")), 1e6, &version) || version < 1 || version > kVersion) {
        return false;
    }
    const QString n = root.value(QStringLiteral("name")).toString().trimmed();
    if (n.isEmpty()) return false;

    isotone::EqState s;
    const QJsonValue auto_preamp = root.value(QStringLiteral("autoPreamp"));
    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    double channels = 0, mask = 0;
    if (!finite(root.value(QStringLiteral("preampDb")), &s.preamp_db) || !auto_preamp.isBool() ||
        !whole(layout.value(QStringLiteral("channels")), isotone::kMaxApoChannels, &channels) ||
        !whole(layout.value(QStringLiteral("speakerMask")), 4294967295.0, &mask) ||
        !root.value(QStringLiteral("bands")).isArray()) {
        return false;
    }
    s.auto_preamp = auto_preamp.toBool();
    s.layout_channels = static_cast<uint32_t>(channels);
    s.layout_speaker_mask = static_cast<uint32_t>(mask);

    for (const QJsonValue& v : root.value(QStringLiteral("bands")).toArray()) {
        if (!v.isObject()) return false;
        const QJsonObject o = v.toObject();
        isotone::Band b;
        double id = 0, band_channels = 0;
        const int type = indexOf(o.value(QStringLiteral("type")), kTypeNames, 8);
        const int mode = indexOf(o.value(QStringLiteral("widthMode")), kWidthNames, 3);
        const QJsonValue corner = o.value(QStringLiteral("shelfCorner"));
        const QJsonValue enabled = o.value(QStringLiteral("enabled"));
        if (!whole(o.value(QStringLiteral("id")), 4294967295.0, &id) || type < 0 || mode < 0 ||
            !finite(o.value(QStringLiteral("fc")), &b.fc) || !finite(o.value(QStringLiteral("gainDb")), &b.gain_db) ||
            !finite(o.value(QStringLiteral("width")), &b.width) || b.fc <= 0 || b.width <= 0 || !corner.isBool() ||
            !enabled.isBool() || !whole(o.value(QStringLiteral("channels")), 4294967295.0, &band_channels)) {
            return false;
        }
        b.id = static_cast<uint32_t>(id);
        b.type = static_cast<isotone::FilterType>(type);
        b.width_mode = static_cast<isotone::WidthMode>(mode);
        b.shelf_corner = corner.toBool();
        b.enabled = enabled.toBool();
        b.channels = static_cast<isotone::ChannelMask>(band_channels);
        s.bands.push_back(b);
    }
    const QJsonValue scope = root.value(QStringLiteral("forOutput"));
    if (!scope.isUndefined() && !scope.isNull() && !scope.isString()) return false;
    if (for_output != nullptr) *for_output = scope.toString();
    *name = n;
    *eq = std::move(s);
    return true;
}

}  // namespace presetfile

isotone::EqState eq_part(const isotone::EqState& state, uint32_t channels, uint32_t speaker_mask) {
    isotone::EqState eq;
    eq.bands = state.bands;
    eq.preamp_db = state.preamp_db;
    eq.auto_preamp = state.auto_preamp;
    eq.layout_channels = channels;
    eq.layout_speaker_mask = speaker_mask;
    return eq;
}

bool same_eq(const isotone::EqState& a, const isotone::EqState& b_in) {
    isotone::EqState b = b_in;
    if (a.layout_channels != 0) remap_channels(&b, isotone::ChannelLayout{a.layout_channels, a.layout_speaker_mask});
    // float32 in a region keeps about 7 digits; Isotone.txt 12.
    constexpr double kRelative = 1e-5, kDb = 1e-4;
    if (a.auto_preamp != b.auto_preamp || a.bands.size() != b.bands.size()) return false;
    if (!a.auto_preamp && std::abs(a.preamp_db - b.preamp_db) > kDb) return false;
    for (size_t i = 0; i < a.bands.size(); ++i) {
        const isotone::Band& x = a.bands[i];
        const isotone::Band& y = b.bands[i];
        if (x.type != y.type || x.width_mode != y.width_mode || x.shelf_corner != y.shelf_corner ||
            x.channels != y.channels || x.enabled != y.enabled || !near(x.fc, y.fc, kRelative) ||
            std::abs(x.gain_db - y.gain_db) > kDb || !near(x.width, y.width, kRelative)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

PresetStore::PresetStore(const QString& data_dir) : data_dir_(data_dir) { reload(); }

QString PresetStore::presetsDir() const { return QDir(data_dir_).filePath(QStringLiteral("presets")); }

void PresetStore::reload() {
    presets_.clear();
    outputs_.clear();
    const QDir dir(presetsDir());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly)) continue;
        Preset p;
        if (!presetfile::read(f.readAll(), &p.name, &p.eq, &p.for_output)) continue;
        p.id = file.chopped(5);
        // Two files with one name (a copied file): the later one is numbered.
        p.name = uniqueName(p.name);
        presets_.push_back(std::move(p));
    }
    sort();

    QFile f(QDir(data_dir_).filePath(QStringLiteral("outputs.json")));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonObject outputs = doc.object().value(QStringLiteral("outputs")).toObject();
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        outputs_[it.key()] = Assignment{o.value(QStringLiteral("preset")).toString(), o.value(QStringLiteral("name")).toString()};
    }
}

void PresetStore::sort() {
    // Qt ignores numeric mode under the C locale (C.UTF-8 is what a bare Linux
    // session, a container or a CI runner has), which puts "Preset 10" before
    // "Preset 9". English collation orders names the same way otherwise.
    QCollator collator(QLocale().language() == QLocale::C ? QLocale(QLocale::English) : QLocale());
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(presets_.begin(), presets_.end(),
                     [&](const Preset& a, const Preset& b) { return collator.compare(a.name, b.name) < 0; });
}

const PresetStore::Preset* PresetStore::byName(const QString& name) const {
    for (const Preset& p : presets_)
        if (p.name == name) return &p;
    return nullptr;
}

const PresetStore::Preset* PresetStore::byId(const QString& id) const {
    for (const Preset& p : presets_)
        if (p.id == id) return &p;
    return nullptr;
}

QString PresetStore::uniqueName(const QString& base_in, const QString& except_id) const {
    const QString base = base_in.trimmed();
    const auto taken = [&](const QString& n) {
        for (const Preset& p : presets_)
            if (p.id != except_id && p.name.compare(n, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    if (base.isEmpty() || !taken(base)) return base;
    static const QRegularExpression numbered(QStringLiteral("^(.*\\S) (\\d+)$"));
    const QRegularExpressionMatch m = numbered.match(base);
    const QString stem = m.hasMatch() ? m.captured(1) : base;
    for (int i = 2;; ++i) {
        const QString candidate = QStringLiteral("%1 %2").arg(stem).arg(i);
        if (!taken(candidate)) return candidate;
    }
}

QString PresetStore::add(const QString& name, const isotone::EqState& eq, const QString& for_output) {
    Preset p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = uniqueName(name);
    p.eq = eq;
    p.for_output = for_output;
    if (p.name.isEmpty() || !QDir().mkpath(presetsDir()) ||
        !writeAtomically(QDir(presetsDir()).filePath(p.id + QStringLiteral(".json")),
                         presetfile::write(p.name, p.eq, p.for_output))) {
        return QString();
    }
    presets_.push_back(std::move(p));
    const QString id = presets_.back().id;
    sort();
    return id;
}

bool PresetStore::setForOutput(const QString& id, const QString& for_output) {
    for (Preset& p : presets_) {
        if (p.id != id) continue;
        if (p.for_output == for_output) return true;
        if (!writeAtomically(QDir(presetsDir()).filePath(id + QStringLiteral(".json")),
                             presetfile::write(p.name, p.eq, for_output)))
            return false;
        p.for_output = for_output;
        return true;
    }
    return false;
}

bool PresetStore::update(const QString& id, const isotone::EqState& eq) {
    for (Preset& p : presets_) {
        if (p.id != id) continue;
        if (!writeAtomically(QDir(presetsDir()).filePath(id + QStringLiteral(".json")),
                             presetfile::write(p.name, eq, p.for_output)))
            return false;
        p.eq = eq;
        return true;
    }
    return false;
}

QString PresetStore::rename(const QString& id, const QString& name) {
    const QString unique = uniqueName(name, id);
    for (Preset& p : presets_) {
        if (p.id != id) continue;
        if (unique.isEmpty()) return QString();
        if (unique == p.name) return unique;
        if (!writeAtomically(QDir(presetsDir()).filePath(id + QStringLiteral(".json")),
                             presetfile::write(unique, p.eq, p.for_output)))
            return QString();
        p.name = unique;
        sort();
        return unique;
    }
    return QString();
}

bool PresetStore::remove(const QString& id) {
    const auto it = std::find_if(presets_.begin(), presets_.end(), [&](const Preset& p) { return p.id == id; });
    if (it == presets_.end()) return false;
    const QString path = QDir(presetsDir()).filePath(id + QStringLiteral(".json"));
    if (QFile::exists(path) && !QFile::remove(path)) return false;
    presets_.erase(it);
    bool assigned = false;
    for (auto& [guid, a] : outputs_) {
        if (a.preset != id) continue;
        a.preset.clear();
        assigned = true;
    }
    // The preset is gone from memory and disk either way. An assignment
    // outputs.json keeps when it cannot be written names no preset, as it reads.
    if (assigned) writeOutputs();
    return true;
}

QString PresetStore::assignment(const QString& guid) const {
    const auto it = outputs_.find(guid);
    return it == outputs_.end() ? QString() : it->second.preset;
}

QString PresetStore::outputName(const QString& guid) const {
    const auto it = outputs_.find(guid);
    return it == outputs_.end() ? QString() : it->second.name;
}

bool PresetStore::assign(const QString& guid, const QString& id, const QString& output_name) {
    if (guid.isEmpty()) return false;
    Assignment& a = outputs_[guid];
    a.preset = id;
    if (!output_name.isEmpty()) a.name = output_name;
    return writeOutputs();
}

bool PresetStore::setOutputName(const QString& guid, const QString& output_name) {
    const auto it = outputs_.find(guid);
    if (it == outputs_.end() || output_name.isEmpty() || it->second.name == output_name) return false;
    it->second.name = output_name;
    return writeOutputs();
}

std::vector<QString> PresetStore::assignedTo(const QString& id) const {
    std::vector<QString> guids;
    for (const auto& [guid, a] : outputs_)
        if (!id.isEmpty() && a.preset == id) guids.push_back(guid);
    return guids;
}

bool PresetStore::writeOutputs() const {
    QJsonObject outputs;
    for (const auto& [guid, a] : outputs_) {
        if (a.preset.isEmpty()) continue;
        outputs.insert(guid, QJsonObject{{QStringLiteral("preset"), a.preset}, {QStringLiteral("name"), a.name}});
    }
    const QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("outputs"), outputs}};
    return QDir().mkpath(data_dir_) &&
           writeAtomically(QDir(data_dir_).filePath(QStringLiteral("outputs.json")), QJsonDocument(root).toJson());
}
