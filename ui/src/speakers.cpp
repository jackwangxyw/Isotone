// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "speakers.h"

#include <QMetaObject>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>

#include "eqsession.h"
#include "isotone/speakers.h"

using isotone::ChannelMask;

namespace {

const char* const kLayoutNames[] = {"Stereo", "2.1", "5.1", "7.1"};

bool layout_by_name(const QString& name, isotone::devices::SpeakerLayout* out) {
    for (int i = 0; i < 4; ++i) {
        if (name == QLatin1String(kLayoutNames[i])) {
            *out = static_cast<isotone::devices::SpeakerLayout>(i);
            return true;
        }
    }
    return false;
}

ChannelMask bit(uint32_t channel) { return channel < isotone::kMaskChannels ? ChannelMask{1} << channel : 0; }

}  // namespace

Speakers::Speakers(EqSession* session, QObject* parent) : QAbstractListModel(parent), session_(session) {
    layout_setter_ = [](const std::wstring& endpoint, isotone::devices::SpeakerLayout layout) {
        isotone::devices::LayoutChange change;
        return isotone::devices::set_speaker_layout(endpoint, layout, &change);
    };
    if (session_) connect(session_, &EqSession::stateChanged, this, &Speakers::sessionChanged);
    reloadOutput();
}

Speakers::~Speakers() { tone_.stop(); }

Speakers* Speakers::create(QQmlEngine* qml, QJSEngine*) {
    return new Speakers(qml->singletonInstance<EqSession*>("Isotone", "EqSession"));
}

void Speakers::setStore(const SpeakerStore& store) {
    store_ = store;
    reloadOutput();
}

const isotone::EqState& Speakers::state() const {
    static const isotone::EqState kFlat;
    return session_ ? session_->state() : kFlat;
}

uint32_t Speakers::speakerMask() const { return mask_; }

int Speakers::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(speakers_.size()); }

QVariant Speakers::data(const QModelIndex& index, int role) const {
    if (!validRow(index.row())) return {};
    const isotone::ui::Speaker& s = speakers_[static_cast<size_t>(index.row())];
    const isotone::SpeakerSetup& sp = state().speakers;
    const uint32_t c = s.channel;
    const double delay = c < isotone::kMaxChannels ? sp.delay_ms[c] : 0.0;
    switch (role) {
        case CodeRole: return QString::fromStdString(s.code);
        case NameRole: return QString::fromStdString(s.name);
        case ChannelRole: return static_cast<int>(c);
        case LevelRole: return c < isotone::kMaxChannels ? state().channel_gain_db[c] : 0.0;
        case DistanceRole: return isotone::ui::distance_for_delay(farthest_, delay);
        case DelayRole: return delay;
        case InvertedRole: return (sp.inverted & bit(c)) != 0;
        case MutedRole: return (sp.muted & bit(c)) != 0;
        case SoloMutedRole:
            return session_ && (session_->liveOverrides().solo_muted & bit(c)) != 0;
        case SoloedRole: return solo_ == index.row();
        case SmallRole: return (sp.small_speakers & bit(c)) != 0;
        case LfeRole: return s.bit == isotone::kSpeakerLowFrequency;
        case PlayingRole: return tones_ && playing_ == index.row();
    }
    return {};
}

QHash<int, QByteArray> Speakers::roleNames() const {
    return {{CodeRole, "code"},         {NameRole, "name"},       {ChannelRole, "channel"},
            {LevelRole, "level"},       {DistanceRole, "distance"}, {DelayRole, "delay"},
            {InvertedRole, "inverted"}, {MutedRole, "muted"},     {SoloMutedRole, "soloMuted"},
            {SoloedRole, "soloed"},     {SmallRole, "small"},     {LfeRole, "lfe"},
            {PlayingRole, "playing"}};
}

// ---------------------------------------------------------------------------
// The output

void Speakers::sessionChanged() {
    const isotone::ui::OutputTarget target = session_ ? session_->target() : isotone::ui::OutputTarget{};
    if (target.guid != guid_ || target.layout.channels != channels_ || target.layout.speaker_mask != mask_) {
        reloadOutput();
        return;
    }
    if (session_ && session_->undoExtra() != farthest_) {   // a distance change undone or redone
        farthest_ = session_->undoExtra();
        if (!store_.setFarthest(isotone::ui::widen_id(guid_), farthest_)) emit saveFailed();
    }
    updateOverrides();   // what solo leaves playing follows the small speakers
    emit setupChanged();
}

void Speakers::reloadOutput() {
    const isotone::ui::OutputTarget target = session_ ? session_->target() : isotone::ui::OutputTarget{};
    // Another output or layout: a tone on the old stream and a solo of its speakers end.
    stopTone();
    const bool had_tones = tones_, had_solo = solo_ >= 0;
    tones_ = false;
    playing_ = -1;
    solo_ = -1;
    if (session_) session_->setLiveOverrides({});

    beginResetModel();
    guid_ = target.guid;
    channels_ = target.layout.channels;
    mask_ = target.layout.speaker_mask;
    speakers_ = channels_ > 2 ? isotone::ui::layout_speakers(channels_, mask_) : std::vector<isotone::ui::Speaker>{};
    user_groups_ = store_.groups(isotone::ui::widen_id(guid_));
    farthest_ = store_.farthest(isotone::ui::widen_id(guid_));
    if (session_) session_->setUndoExtra(farthest_, true);
    endResetModel();
    supported_.clear();
    if (session_) session_->setUserGroups(user_groups_);
    showing_ = QStringLiteral("all");
    if (session_) session_->setShowingMask(0);

    emit layoutChanged();
    emit supportedLayoutsChanged();
    emit groupsChanged();
    emit setupChanged();
    emit showingChanged();
    if (had_tones) emit tonesChanged();
    if (had_solo) emit soloChanged();
}

void Speakers::rowsChanged() {
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1));
}

int Speakers::channels() const { return static_cast<int>(channels_); }

QString speaker_layout_name(uint32_t channels, uint32_t speaker_mask) {
    for (const isotone::devices::SpeakerLayoutSpec& spec : isotone::devices::kSpeakerLayouts) {
        const uint32_t mask = speaker_mask != 0 ? speaker_mask : spec.mask;
        if (spec.channels == channels && spec.mask == mask) return QLatin1String(kLayoutNames[static_cast<int>(spec.layout)]);
    }
    // Another arrangement of the same speakers with an LFE (5.1 with back rather
    // than side speakers, 0x3F; 7.1 wide, 0xFF) is still named by its count.
    for (const isotone::devices::SpeakerLayoutSpec& spec : isotone::devices::kSpeakerLayouts) {
        if (spec.channels == channels && channels > 2 && (speaker_mask & 0x8) != 0)
            return QLatin1String(kLayoutNames[static_cast<int>(spec.layout)]);
    }
    return QStringLiteral("%1 ch").arg(channels);
}

QString Speakers::layoutName() const { return speaker_layout_name(channels_, mask_); }

int Speakers::layoutChannels(const QString& name) const {
    isotone::devices::SpeakerLayout layout;
    return layout_by_name(name, &layout) ? isotone::devices::speaker_layout_spec(layout)->channels : 0;
}

void Speakers::refreshSupportedLayouts() {
    QStringList names;
    std::vector<isotone::devices::SpeakerLayout> layouts;
    if (!guid_.empty() && SUCCEEDED(isotone::devices::supported_speaker_layouts(isotone::ui::widen_id(guid_), &layouts))) {
        for (isotone::devices::SpeakerLayout l : layouts) names << QLatin1String(kLayoutNames[static_cast<int>(l)]);
    }
    if (names == supported_) return;
    supported_ = names;
    emit supportedLayoutsChanged();
}

int Speakers::setLayout(const QString& name) {
    isotone::devices::SpeakerLayout layout;
    if (!layout_by_name(name, &layout)) return E_INVALIDARG;
    if (guid_.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    // A tone's stream is on the old format.
    setTestTones(false);
    return layout_setter_(isotone::ui::widen_id(guid_), layout);
}

QVariantList Speakers::speakerList() const {
    QVariantList out;
    for (const isotone::ui::Speaker& s : speakers_) {
        out.append(QVariantMap{{QStringLiteral("code"), QString::fromStdString(s.code)},
                               {QStringLiteral("name"), QString::fromStdString(s.name)},
                               {QStringLiteral("channel"), static_cast<int>(s.channel)},
                               {QStringLiteral("lfe"), s.bit == isotone::kSpeakerLowFrequency}});
    }
    return out;
}

QVariantList Speakers::groups() const {
    QVariantList out;
    if (channels_ <= 2) return out;
    for (const isotone::ui::ResolvedGroup& g : isotone::ui::layout_groups(channels_, mask_, user_groups_)) {
        // Built-in groups list their speakers in the prototype's order, the user's in channel order.
        std::vector<std::string> order;
        if (g.builtin && g.name == "Front") order = {"L", "C", "R"};
        if (g.builtin && g.name == "Surround") order = {"SL", "SR", "RL", "RR"};
        for (const isotone::ui::Speaker& s : speakers_)
            if (std::find(order.begin(), order.end(), s.code) == order.end()) order.push_back(s.code);
        QString codes;
        for (const std::string& code : order) {
            const auto it = std::find_if(speakers_.begin(), speakers_.end(), [&](const isotone::ui::Speaker& s) { return s.code == code; });
            if (it != speakers_.end() && (g.mask & bit(it->channel)) != 0)
                codes += (codes.isEmpty() ? QString() : QStringLiteral(" ")) + QString::fromStdString(code);
        }
        if (g.builtin && g.name == "All") codes = QStringLiteral("%1 speakers").arg(speakers_.size());
        out.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(g.name)},
                               {QStringLiteral("mask"), static_cast<int>(g.mask)},
                               {QStringLiteral("codes"), codes},
                               {QStringLiteral("builtin"), g.builtin}});
    }
    return out;
}

bool Speakers::addGroup(const QString& name, const QStringList& codes) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char(':'))) return false;
    for (const QVariant& g : groups())
        if (g.toMap().value(QStringLiteral("name")).toString().compare(trimmed, Qt::CaseInsensitive) == 0) return false;
    isotone::ui::SpeakerGroup group;
    group.name = trimmed.toStdString();
    for (const QString& c : codes) group.codes.push_back(c.toStdString());
    if (isotone::ui::codes_mask(group.codes, channels_, mask_) == 0) return false;
    user_groups_.push_back(group);
    if (!store_.setGroups(isotone::ui::widen_id(guid_), user_groups_)) emit saveFailed();
    if (session_) session_->setUserGroups(user_groups_);
    emit groupsChanged();
    return true;
}

void Speakers::removeGroup(const QString& name) {
    const auto it = std::find_if(user_groups_.begin(), user_groups_.end(),
                                 [&](const isotone::ui::SpeakerGroup& g) { return g.name == name.toStdString(); });
    if (it == user_groups_.end()) return;
    user_groups_.erase(it);
    if (!store_.setGroups(isotone::ui::widen_id(guid_), user_groups_)) emit saveFailed();
    if (session_) session_->setUserGroups(user_groups_);
    if (showing_ == QStringLiteral("group:") + name) setShowing(QStringLiteral("all"));
    emit groupsChanged();
    emit showingChanged();
}

// ---------------------------------------------------------------------------
// The setup

void Speakers::edit(const std::function<void(isotone::EqState*)>& change) {
    if (!session_) return;
    session_->editSpeakers(change);   // stateChanged: the rows and properties follow
    updateOverrides();                // a small speaker changes what solo leaves playing
}

double Speakers::crossoverHz() const { return state().speakers.crossover_hz; }
double Speakers::lfeLowpassHz() const { return state().speakers.lfe_lowpass_hz; }
bool Speakers::bassManagement() const { return state().speakers.bass_management; }
int Speakers::smallSpeakers() const { return static_cast<int>(state().speakers.small_speakers); }
int Speakers::upmix() const { return static_cast<int>(state().speakers.upmix); }
bool Speakers::swapFrontRear() const { return state().speakers.swap_front_rear; }
bool Speakers::swapLeftRight() const { return state().speakers.swap_left_right; }
double Speakers::lipSyncMs() const { return state().speakers.lip_sync_ms; }

void Speakers::setCrossoverHz(double hz) {
    const double v = isotone::ui::snap_crossover_hz(hz);
    if (!std::isfinite(v)) return;
    edit([v](isotone::EqState* s) { s->speakers.crossover_hz = v; });
}

void Speakers::setLfeLowpassHz(double hz) {
    const double v = isotone::ui::snap_lfe_lowpass_hz(hz);
    if (!std::isfinite(v)) return;
    edit([v](isotone::EqState* s) { s->speakers.lfe_lowpass_hz = v; });
}

void Speakers::setUpmix(int upmix) {
    if (upmix < 0 || upmix > 2) return;
    edit([upmix](isotone::EqState* s) { s->speakers.upmix = static_cast<isotone::Upmix>(upmix); });
}

void Speakers::setSwapFrontRear(bool on) {
    edit([on](isotone::EqState* s) { s->speakers.swap_front_rear = on; });
}

void Speakers::setSwapLeftRight(bool on) {
    edit([on](isotone::EqState* s) { s->speakers.swap_left_right = on; });
}

void Speakers::setLipSyncMs(double ms) {
    if (!std::isfinite(ms)) return;
    const double v = std::clamp(ms, 0.0, isotone::ui::kLipSyncMaxMs);
    edit([v](isotone::EqState* s) { s->speakers.lip_sync_ms = v; });
}

void Speakers::setLevel(int row, double db) {
    if (!validRow(row) || !std::isfinite(db)) return;
    const uint32_t c = speakers_[static_cast<size_t>(row)].channel;
    if (c >= isotone::kMaxChannels) return;
    const double v = std::clamp(db, isotone::ui::kLevelMinDb, isotone::ui::kLevelMaxDb);
    edit([c, v](isotone::EqState* s) { s->channel_gain_db[c] = v; });
}

void Speakers::setDistance(int row, double metres) {
    if (!validRow(row)) return;
    const uint32_t c = speakers_[static_cast<size_t>(row)].channel;
    edit([&](isotone::EqState* s) {
        isotone::ui::set_speaker_distance(&s->speakers, channels_, &farthest_, c, metres);
        session_->setUndoExtra(farthest_, false);
    });
    if (!store_.setFarthest(isotone::ui::widen_id(guid_), farthest_)) emit saveFailed();
    rowsChanged();
}

void Speakers::setDelay(int row, double ms) {
    if (!validRow(row)) return;
    const uint32_t c = speakers_[static_cast<size_t>(row)].channel;
    edit([&](isotone::EqState* s) {
        isotone::ui::set_speaker_delay(&s->speakers, &farthest_, c, ms);
        session_->setUndoExtra(farthest_, false);
    });
    if (!store_.setFarthest(isotone::ui::widen_id(guid_), farthest_)) emit saveFailed();
    rowsChanged();
}

void Speakers::setInverted(int row, bool on) {
    if (!validRow(row)) return;
    const ChannelMask b = bit(speakers_[static_cast<size_t>(row)].channel);
    edit([b, on](isotone::EqState* s) { s->speakers.inverted = on ? s->speakers.inverted | b : s->speakers.inverted & ~b; });
}

void Speakers::setMuted(int row, bool on) {
    if (!validRow(row)) return;
    const ChannelMask b = bit(speakers_[static_cast<size_t>(row)].channel);
    edit([b, on](isotone::EqState* s) { s->speakers.muted = on ? s->speakers.muted | b : s->speakers.muted & ~b; });
}

void Speakers::setSmall(int row, bool on) {
    if (!validRow(row) || speakers_[static_cast<size_t>(row)].bit == isotone::kSpeakerLowFrequency) return;
    const ChannelMask b = bit(speakers_[static_cast<size_t>(row)].channel);
    edit([b, on](isotone::EqState* s) {
        s->speakers.small_speakers = on ? s->speakers.small_speakers | b : s->speakers.small_speakers & ~b;
        s->speakers.bass_management = s->speakers.small_speakers != 0;
    });
}

// ---------------------------------------------------------------------------
// Solo and test tones

void Speakers::updateOverrides() {
    if (!session_) return;
    const int solo_channel = validRow(solo_) ? static_cast<int>(speakers_[static_cast<size_t>(solo_)].channel) : -1;
    session_->setLiveOverrides({isotone::ui::solo_mute_mask(state().speakers, channels_, mask_, solo_channel), tones_});
    rowsChanged();
}

void Speakers::toggleSolo(int row) {
    if (!validRow(row)) return;
    solo_ = solo_ == row ? -1 : row;
    updateOverrides();
    emit soloChanged();
}

void Speakers::setDistanceMode(bool on) {
    if (on == distance_mode_) return;
    distance_mode_ = on;
    emit distanceModeChanged();
}

void Speakers::setTestTones(bool on) {
    if (on == tones_ || (on && speakers_.empty())) return;
    if (on) {
        // The state goes out bypassed before the noise does.
        tones_ = true;
        playing_ = 0;
        updateOverrides();
        startTone();
    } else {
        stopTone();
        tones_ = false;
        playing_ = -1;
        updateOverrides();   // the real state again
    }
    emit tonesChanged();
}

void Speakers::toggleTone(int row) {
    if (!tones_ || !validRow(row)) return;
    stopTone();
    playing_ = playing_ == row ? -1 : row;
    startTone();
    rowsChanged();
    emit tonesChanged();
}

void Speakers::startTone() {
    // No output behind the session (a test): nothing to play on.
    if (!validRow(playing_) || guid_.empty() || !session_ || session_->target().backend == isotone::ui::Backend::none) return;
    QPointer<Speakers> self(this);
    const quint64 generation = ++tone_generation_;
    tone_.start(isotone::ui::widen_id(guid_), speakers_[static_cast<size_t>(playing_)].channel, [self, generation](HRESULT hr, const char*) {
        QMetaObject::invokeMethod(
            self.data(),
            [self, hr, generation] {
                // A tone stopped since, and its stream with it, is not the one playing.
                if (!self || self->tone_generation_ != generation || !self->tones_) return;
                self->setTestTones(false);
                emit self->toneFailed(static_cast<int>(hr));
            },
            Qt::QueuedConnection);
    });
}

void Speakers::stopTone() {
    tone_.stop();
    ++tone_generation_;
}

void Speakers::endSession() {
    setTestTones(false);
    if (solo_ >= 0) toggleSolo(solo_);
}

// ---------------------------------------------------------------------------
// Showing

ChannelMask Speakers::showingMask(const QString& key, bool* valid) const {
    *valid = true;
    if (key == QLatin1String("all")) return 0;
    if (key.startsWith(QLatin1String("group:"))) {
        const std::string name = key.mid(6).toStdString();
        for (const isotone::ui::ResolvedGroup& g : isotone::ui::layout_groups(channels_, mask_, user_groups_))
            if (g.name == name && g.name != "All") return g.mask;
    }
    if (key.startsWith(QLatin1String("speaker:"))) {
        bool ok = false;
        const uint channel = key.mid(8).toUInt(&ok);
        if (ok && channel < speakers_.size()) return bit(channel);
    }
    *valid = false;
    return 0;
}

void Speakers::setShowing(const QString& key) {
    bool valid = false;
    const ChannelMask mask = showingMask(key, &valid);
    const QString k = valid && channels_ > 2 ? key : QStringLiteral("all");
    if (session_) session_->setShowingMask(static_cast<int>(valid ? mask : 0));
    if (k == showing_) return;
    showing_ = k;
    emit showingChanged();
}

QString Speakers::showingLabel() const {
    for (const QVariant& item : showingItems()) {
        const QVariantMap m = item.toMap();
        if (m.value(QStringLiteral("key")).toString() == showing_) return m.value(QStringLiteral("label")).toString();
    }
    return QStringLiteral("All speakers");
}

QVariantList Speakers::showingItems() const {
    QVariantList out;
    if (channels_ <= 2) return out;
    const auto item = [](const QString& key, const QString& label, const QString& detail, bool separator) {
        return QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("label"), label}, {QStringLiteral("detail"), detail},
                           {QStringLiteral("separator"), separator}};
    };
    out.append(item(QStringLiteral("all"), QStringLiteral("All speakers"), QString::number(speakers_.size()), false));
    for (const QVariant& g : groups()) {
        const QVariantMap m = g.toMap();
        const QString name = m.value(QStringLiteral("name")).toString();
        if (name == QLatin1String("All") && m.value(QStringLiteral("builtin")).toBool()) continue;
        out.append(item(QStringLiteral("group:") + name, name, m.value(QStringLiteral("codes")).toString(), false));
    }
    bool first = true;
    for (const isotone::ui::Speaker& s : speakers_) {
        out.append(item(QStringLiteral("speaker:%1").arg(s.channel), QString::fromStdString(s.name), QString::fromStdString(s.code), first));
        first = false;
    }
    return out;
}
