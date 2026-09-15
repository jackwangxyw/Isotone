// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "eqsession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <set>

#include "apppaths.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/response.h"
#include "output_state.h"
#include "outputs.h"
#include "persisted_state.h"
#include "typed_value.h"

static_assert(static_cast<int>(EqSession::Plain) == static_cast<int>(isotone::ui::TypedUnit::Plain) &&
              static_cast<int>(EqSession::SlopeDb) == static_cast<int>(isotone::ui::TypedUnit::SlopeDb) &&
              static_cast<int>(EqSession::Milliseconds) == static_cast<int>(isotone::ui::TypedUnit::Milliseconds));

namespace {

constexpr int kBandColours = 12;
// The spectrum disappears when no audio has come for this long (engine idle).
constexpr qint64 kSpectrumTimeoutMs = 500;

QString type_name(isotone::FilterType t) {
    switch (t) {
        case isotone::FilterType::Peaking: return QStringLiteral("Peak");
        case isotone::FilterType::LowShelf: return QStringLiteral("Low shelf");
        case isotone::FilterType::HighShelf: return QStringLiteral("High shelf");
        case isotone::FilterType::LowPass: return QStringLiteral("Low pass");
        case isotone::FilterType::HighPass: return QStringLiteral("High pass");
        case isotone::FilterType::BandPass: return QStringLiteral("Band pass");
        case isotone::FilterType::Notch: return QStringLiteral("Notch");
        case isotone::FilterType::AllPass: return QStringLiteral("All pass");
    }
    return {};
}

double band_q(const isotone::Band& b) { return b.width; }

// The processor designs only these with the band's gain.
bool uses_gain(isotone::FilterType t) {
    return t == isotone::FilterType::Peaking || t == isotone::FilterType::LowShelf ||
           t == isotone::FilterType::HighShelf;
}

// Which of a stereo output's channels a band is on: 0 left, 1 right, 2 both.
int stereo_channels(const isotone::Band& b) {
    const isotone::ChannelMask m = b.channels & 0x3;
    return m == 0x1 ? 0 : m == 0x2 ? 1 : 2;
}

// A loaded preamp this close to Auto's value is Auto's: Isotone.txt rounds it.
constexpr double kAutoPreampMatchDb = 0.01;

}  // namespace

EqSession::EqSession(QObject* parent)
    : EqSession(std::make_unique<isotone::ui::DeviceLink>(L"Global\\", AppPaths::compatConfigDir().toStdWString()), parent) {}

EqSession::EqSession(std::unique_ptr<isotone::ui::DeviceLink> link, QObject* parent)
    : QAbstractListModel(parent), link_(std::move(link)), audio_(size_t{8192} * isotone::kMaxChannels) {
    state_.auto_preamp = true;
    baseline_.state = state_;
    preset_name_ = QStringLiteral("Untitled");
    rebuildOrder();
    clock_.start();
    spectrum_timer_.setInterval(16);
    spectrum_timer_.setTimerType(Qt::PreciseTimer);
    connect(&spectrum_timer_, &QTimer::timeout, this, &EqSession::readSpectrum);
    spectrum_timer_.start();
}

EqSession::~EqSession() = default;

int EqSession::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(order_.size());
}

const isotone::Band* EqSession::bandAt(int row) const {
    if (row < 0 || row >= static_cast<int>(order_.size())) return nullptr;
    return &state_.bands[order_[static_cast<size_t>(row)]];
}

int EqSession::selectedRow() const {
    for (int row = 0; row < rowCount(); ++row) {
        if (bandAt(row)->id == selected_id_) return row;
    }
    return -1;
}

QVariant EqSession::data(const QModelIndex& index, int role) const {
    const isotone::Band* b = bandAt(index.row());
    if (b == nullptr) return {};
    switch (role) {
        case BandIdRole: return b->id;
        case PositionRole: return index.row() + 1;
        case TypeNameRole: return type_name(b->type);
        case FrequencyRole: return b->fc;
        case GainRole: return b->gain_db;
        case QRole: return band_q(*b);
        case EnabledRole: return b->enabled;
        case TargetRole: {
            if (target_.layout.channels > 2) {
                return QString::fromStdString(isotone::ui::target_label(b->channels, target_.layout.channels,
                                                                        target_.layout.speaker_mask, user_groups_));
            }
            static const QString kNames[] = {QStringLiteral("L"), QStringLiteral("R"), QStringLiteral("L+R")};
            return kNames[stereo_channels(*b)];
        }
        case ChannelsRole: return stereo_channels(*b);
        case ChannelMaskRole: return static_cast<int>(b->channels);
        case TypeRole: return static_cast<int>(b->type);
        case ColorIndexRole: return static_cast<int>((b->id - 1) % kBandColours);
        case SelectedRole: return b->id == selected_id_;
        case WidthLabelRole:
            switch (b->width_mode) {
                case isotone::WidthMode::Q: return QStringLiteral("Q %1").arg(b->width, 0, 'f', 2);
                case isotone::WidthMode::BandwidthOct: return QStringLiteral("%1 oct").arg(b->width, 0, 'f', 2);
                case isotone::WidthMode::SlopeDb: return QStringLiteral("%1 dB/oct").arg(b->width, 0, 'f', 1);
            }
            return {};
        case WidthUnitRole:
            switch (b->width_mode) {
                case isotone::WidthMode::Q: return Q;
                case isotone::WidthMode::BandwidthOct: return Octaves;
                case isotone::WidthMode::SlopeDb: return SlopeDb;
            }
            return {};
        case HasGainRole: return uses_gain(b->type);
    }
    return {};
}

QHash<int, QByteArray> EqSession::roleNames() const {
    return {{BandIdRole, "bandId"},      {PositionRole, "position"}, {TypeNameRole, "typeName"},
            {FrequencyRole, "frequency"}, {GainRole, "gain"},         {QRole, "q"},
            {EnabledRole, "bandEnabled"}, {TargetRole, "target"},     {ColorIndexRole, "colorIndex"},
            {SelectedRole, "selected"},   {WidthLabelRole, "widthLabel"}, {WidthUnitRole, "widthUnit"},
            {HasGainRole, "hasGain"},     {TypeRole, "type"},         {ChannelsRole, "channels"},
            {ChannelMaskRole, "channelMask"}};
}

std::vector<size_t> EqSession::displayOrder() const {
    std::vector<size_t> order(state_.bands.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (by_frequency_) {
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return state_.bands[a].fc < state_.bands[b].fc; });
    }
    return order;
}

void EqSession::rebuildOrder() { order_ = displayOrder(); }

// ---------------------------------------------------------------------------
// The output

void EqSession::useOutput(Outputs* outputs) {
    const Outputs::Output* o = outputs ? outputs->current() : nullptr;
    isotone::ui::OutputTarget target;
    if (o) target = isotone::ui::OutputTarget{o->guid, o->backend, o->layout};
    useTarget(target);
}

void EqSession::useTarget(const isotone::ui::OutputTarget& target) {
    const bool same = target.guid == target_.guid && target.backend == target_.backend;
    const isotone::ui::OutputLayout before = target_.layout;
    // Solo and test tones end here, on the output and layout they were written
    // for: its real state, before the link moves on.
    if (overrides_.solo_muted != 0 || overrides_.test_tones) {
        overrides_ = {};
        write();
    }
    target_ = target;
    link_->set_target(target_);
    link_->take_region_opened();   // what is loaded below is what the region holds
    analyzer_.reset();
    if (same) {   // a format change on the same output keeps what is being edited
        // In another layout the speaker values move by speaker role, before they
        // are shown or edited (ui-spec.md, "A state knows its layout").
        if (target_.layout.channels != before.channels || target_.layout.speaker_mask != before.speaker_mask) {
            if (state_.layout_channels == 0) {
                state_.layout_channels = before.channels;
                state_.layout_speaker_mask = before.speaker_mask;
            }
            isotone::remap_channels(&state_, isotone::ChannelLayout{target_.layout.channels, target_.layout.speaker_mask});
            showing_mask_ = 0;
            if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1));
            updateAutoPreamp();
            // Not a step: the remap is lossy, so no step before it can be undone onto the new layout.
            write();
            resetHistory();
            emit committed();
            emit viewChanged();
        }
        emit stateChanged();
        emit curveChanged();
        return;
    }

    // Start from what the output plays.
    isotone::EqState loaded;
    loadState(link_->load_current(&loaded) ? &loaded : nullptr);
    emit targetChanged();
}

void EqSession::loadState(const isotone::EqState* state) {
    beginResetModel();
    if (state) {
        isotone::EqState loaded = *state;
        limitBands(&loaded.bands);
        // Written for another layout: moved to the output's, as the engine plays it.
        isotone::remap_channels(&loaded, isotone::ChannelLayout{target_.layout.channels, target_.layout.speaker_mask});
        balance_ = isotone::ui::balance_from_state(loaded, target_.layout.channels);
        isotone::ui::clear_balance(target_.layout.channels, &loaded);
        // Ids must be unique; a file's bands carry none.
        std::set<uint32_t> seen;
        uint32_t next = 1;
        for (const isotone::Band& b : loaded.bands) next = std::max(next, b.id + 1);
        for (isotone::Band& b : loaded.bands) {
            if (b.id == 0 || !seen.insert(b.id).second) {
                b.id = next++;
                seen.insert(b.id);
            }
        }
        state_ = loaded;
        // The output does not carry the mode. Auto unless the preamp was set by hand.
        state_.auto_preamp = std::abs(autoPreampValue() - state_.preamp_db) <= kAutoPreampMatchDb;
    } else {
        state_ = isotone::EqState{};
        state_.auto_preamp = true;
        balance_ = 0.0;
    }
    selected_id_ = state_.bands.empty() ? 0 : state_.bands.front().id;
    rebuildOrder();
    endResetModel();
    resetHistory();
    emit countChanged();
    emit selectionChanged();
    emit stateChanged();
    emit curveChanged();
}

void EqSession::push() {
    if (target_.backend == isotone::ui::Backend::none) return;
    link_->apply(engineState());
}

void EqSession::commit() {
    record();
    write();
    emit committed();
}

void EqSession::write() {
    if (target_.backend == isotone::ui::Backend::none) return;
    link_->commit(engineState());
}

void EqSession::readSpectrum() {
    const qint64 now = clock_.elapsed();
    uint32_t channels = 0;
    double rate = 48000.0;
    for (int i = 0; i < 8; ++i) {
        const uint32_t frames = link_->read_audio(audio_.data(), 8192, &channels, &rate);
        if (frames == 0) break;
        analyzer_.push(audio_.data(), frames, channels);
        last_frame_ms_ = now;
    }
    // An engine that started playing since the last edit gets the current state.
    if (link_->take_region_opened()) write();
    const bool active = now - last_frame_ms_ < kSpectrumTimeoutMs;
    if (active) analyzer_.update(rate, std::max<qint64>(1, now - last_update_ms_) / 1000.0);
    last_update_ms_ = now;
    if (active || active != spectrum_active_) {
        spectrum_active_ = active;
        emit spectrumChanged();
    }
}

bool EqSession::spectrumLevels(const double* freqs, size_t n, double* out_db) const {
    if (!spectrum_active_) return false;
    analyzer_.levels_at(freqs, n, out_db);
    return true;
}

// Settings, General, Spectrum.
bool EqSession::spectrumPeakLevels(const double* freqs, size_t n, double* out_db) const {
    if (!spectrum_active_) return false;
    analyzer_.peak_levels_at(freqs, n, out_db);
    return true;
}

void EqSession::setSpectrumOptions(int fftSize, double releaseMs, double tiltDbPerOct) {
    if ((fftSize == 4096 || fftSize == 8192 || fftSize == 16384) && static_cast<size_t>(fftSize) != analyzer_.fft_size())
        analyzer_.set_fft_size(static_cast<size_t>(fftSize));
    if (std::isfinite(releaseMs) && releaseMs > 0) analyzer_.set_release_ms(releaseMs);
    if (std::isfinite(tiltDbPerOct)) analyzer_.set_tilt(tiltDbPerOct);
    emit spectrumChanged();
}

// ---------------------------------------------------------------------------
// Edits

void EqSession::setByFrequency(bool on) {
    if (on == by_frequency_) return;
    beginResetModel();
    by_frequency_ = on;
    rebuildOrder();
    endResetModel();
    emit orderChanged();
    emit selectionChanged();
    emit curveChanged();   // handle numbers follow the order
}

void EqSession::setViewChannel(int view) {
    if (view < 0 || view > 2 || view == view_channel_) return;
    view_channel_ = view;
    emit viewChanged();
    emit curveChanged();
}

void EqSession::bandChanged(int row, const QList<int>& roles) {
    emit dataChanged(index(row), index(row), roles);
    updateAutoPreamp();
    push();
    emit curveChanged();
}

double EqSession::autoPreampValue() const {
    static const std::vector<double> grid = isotone::log_grid(20.0, 20000.0, 512);
    const double rate = target_.layout.sample_rate > 0 ? target_.layout.sample_rate : 48000.0;
    // As the bands play with EQ on: turning EQ back on must not clip.
    isotone::EqState probe = state_;
    probe.bypass = false;
    return isotone::auto_preamp_db(probe, target_.layout.channels, target_.layout.speaker_mask, grid.data(),
                                   grid.size(), rate);
}

void EqSession::updateAutoPreamp() {
    if (!state_.auto_preamp) return;
    const double db = autoPreampValue();
    if (db != state_.preamp_db) {
        state_.preamp_db = db;
        emit stateChanged();
    }
}

void EqSession::select(int row) {
    const isotone::Band* b = bandAt(row);
    if (b == nullptr || b->id == selected_id_) return;
    const int previous = selectedRow();
    selected_id_ = b->id;
    baseline_selected_ = b->id;   // the band an edit that follows is undone onto
    if (previous >= 0) emit dataChanged(index(previous), index(previous), {SelectedRole});
    emit dataChanged(index(row), index(row), {SelectedRole});
    emit selectionChanged();
    emit curveChanged();
}

void EqSession::setGain(int row, double db) {
    if (bandAt(row) == nullptr || !std::isfinite(db) || !uses_gain(bandAt(row)->type)) return;
    state_.bands[order_[static_cast<size_t>(row)]].gain_db = std::clamp(db, -24.0, 24.0);
    bandChanged(row, {GainRole});
}

void EqSession::setFrequency(int row, double hz) {
    if (bandAt(row) == nullptr || !std::isfinite(hz)) return;
    state_.bands[order_[static_cast<size_t>(row)]].fc = std::clamp(hz, 10.0, 22000.0);
    bandChanged(row, {FrequencyRole});
}

void EqSession::setWidth(int row, double width, bool commitNow) {
    if (bandAt(row) == nullptr || !std::isfinite(width)) return;
    isotone::Band& b = state_.bands[order_[static_cast<size_t>(row)]];
    if (b.width_mode == isotone::WidthMode::Q) {
        b.width = std::clamp(width, 0.1, 50.0);
    } else {
        // A bandwidth or slope stays one: in its own unit, held where the processor holds it.
        b.width = width;
        b.width = isotone::effective_band(b).width;
    }
    bandChanged(row, {QRole, WidthLabelRole});
    if (commitNow) commit();   // a typed width is a whole edit
}

void EqSession::setEnabled(int row, bool on) {
    if (bandAt(row) == nullptr) return;
    state_.bands[order_[static_cast<size_t>(row)]].enabled = on;
    bandChanged(row, {EnabledRole});
    commit();
}

void EqSession::setType(int row, int type) {
    if (bandAt(row) == nullptr || type < static_cast<int>(isotone::FilterType::Peaking) ||
        type > static_cast<int>(isotone::FilterType::HighShelf)) {
        return;
    }
    isotone::Band& b = state_.bands[order_[static_cast<size_t>(row)]];
    const auto shelf = [](isotone::FilterType t) {
        return t == isotone::FilterType::LowShelf || t == isotone::FilterType::HighShelf;
    };
    const isotone::FilterType to = static_cast<isotone::FilterType>(type);
    if (b.width_mode == isotone::WidthMode::SlopeDb && shelf(b.type) && !shelf(to)) {
        // The processor reads a slope as a Q off a shelf. The Q of the same
        // shelf shape, with the slope and gain the processor held (biquad.cpp).
        const isotone::Band held = isotone::effective_band(b);
        const double a = std::pow(10.0, held.gain_db / 40.0);
        const double inner = (a + 1.0 / a) * (12.0 / held.width - 1.0) + 2.0;
        b.width = 1.0 / std::sqrt(std::max(inner, 0.01));
        b.width_mode = isotone::WidthMode::Q;
    }
    b.type = to;
    b.width = isotone::effective_band(b).width;
    bandChanged(row, {TypeRole, TypeNameRole, HasGainRole, QRole, WidthLabelRole, WidthUnitRole});
    commit();
}

void EqSession::setChannels(int row, int which) {
    if (bandAt(row) == nullptr) return;
    static constexpr isotone::ChannelMask kMasks[] = {0x1, 0x2, isotone::kAllChannels};
    state_.bands[order_[static_cast<size_t>(row)]].channels =
        which >= 0 && which < 3 ? kMasks[which] : isotone::kAllChannels;
    bandChanged(row, {TargetRole, ChannelsRole});
    commit();
}

void EqSession::duplicateBand(int row) {
    if (bandAt(row) == nullptr || !canAddBand()) return;
    isotone::Band copy = *bandAt(row);
    copy.id = nextBandId();
    insertBand(order_[static_cast<size_t>(row)] + 1, copy);
}

void EqSession::resetGain(int row) {
    setGain(row, 0.0);
    commit();
}

bool EqSession::canAddBand() const { return state_.bands.size() < isotone::kParamMaxBands; }

void EqSession::limitBands(std::vector<isotone::Band>* bands) const {
    // The region holds kParamMaxBands (engine contract): an IsoAPO output plays the first ones.
    if (target_.backend == isotone::ui::Backend::native && bands->size() > isotone::kParamMaxBands)
        bands->resize(isotone::kParamMaxBands);
}

uint32_t EqSession::nextBandId() const {
    uint32_t id = 1;
    for (const isotone::Band& b : state_.bands) id = std::max(id, b.id + 1);
    return id;
}

void EqSession::addBand(double hz, double db) {
    if (!canAddBand() || !std::isfinite(hz) || !std::isfinite(db)) return;
    isotone::Band b;
    b.id = nextBandId();
    b.type = isotone::FilterType::Peaking;
    b.fc = std::clamp(hz, 10.0, 22000.0);
    b.gain_db = std::clamp(std::round(db * 10.0) / 10.0, -24.0, 24.0);
    b.width = 1.41;
    insertBand(state_.bands.size(), b);
}

void EqSession::insertBand(size_t at, const isotone::Band& b) {
    beginResetModel();
    state_.bands.insert(state_.bands.begin() + static_cast<std::ptrdiff_t>(at), b);
    selected_id_ = b.id;
    rebuildOrder();
    endResetModel();
    emit countChanged();
    emit selectionChanged();
    updateAutoPreamp();
    commit();
    emit curveChanged();
}

void EqSession::deleteBand(int row) {
    if (bandAt(row) == nullptr) return;
    const size_t at = order_[static_cast<size_t>(row)];
    beginResetModel();
    state_.bands.erase(state_.bands.begin() + static_cast<std::ptrdiff_t>(at));
    rebuildOrder();
    const int next = std::min(row, rowCount() - 1);
    selected_id_ = next >= 0 ? bandAt(next)->id : 0;
    endResetModel();
    emit countChanged();
    emit selectionChanged();
    updateAutoPreamp();
    commit();
    emit curveChanged();
    emit bandDeleted(row + 1, undo_.empty() ? 0 : undo_.back().id);
}

void EqSession::finishEdit() {
    commit();
    // By frequency, a band that passed another changes place when the drag ends,
    // not under the pointer; its id, colour and selection stay.
    if (!by_frequency_ || displayOrder() == order_) return;
    beginResetModel();
    rebuildOrder();
    endResetModel();
    emit selectionChanged();
    emit curveChanged();
}

double EqSession::parseValue(const QString& text, Unit unit) const {
    const QByteArray utf8 = text.toUtf8();
    const std::optional<double> v = isotone::ui::parse_typed_value(
        std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())), static_cast<isotone::ui::TypedUnit>(unit));
    return v ? *v : std::numeric_limits<double>::quiet_NaN();
}

void EqSession::setPreampDb(double db) {
    if (!std::isfinite(db)) return;
    state_.auto_preamp = false;   // moving or typing the preamp turns Auto off
    state_.preamp_db = std::clamp(db, -24.0, 6.0);
    push();
    emit stateChanged();
}

void EqSession::setAutoPreamp(bool on) {
    if (on == state_.auto_preamp) return;
    state_.auto_preamp = on;
    updateAutoPreamp();
    commit();
    emit stateChanged();
}

void EqSession::setBalance(double b) {
    if (!std::isfinite(b)) return;
    balance_ = std::round(std::clamp(b, -1.0, 1.0) * 10.0) / 10.0;
    push();
    emit stateChanged();
}

void EqSession::setMuted(bool on) {
    if (on == state_.mute) return;
    state_.mute = on;
    commit();
    emit stateChanged();
    emit curveChanged();
}

void EqSession::setEqOn(bool on) {
    if (on == !state_.bypass) return;
    state_.bypass = !on;
    commit();
    emit stateChanged();
    emit curveChanged();
}

// ---------------------------------------------------------------------------
// Surround

void EqSession::setShowingMask(int mask) {
    const isotone::ChannelMask m = static_cast<isotone::ChannelMask>(mask);
    if (m == showing_mask_) return;
    showing_mask_ = m;
    emit viewChanged();
    emit curveChanged();
}

void EqSession::setChannelMask(int row, int mask) {
    if (bandAt(row) == nullptr) return;
    const isotone::ChannelMask all = isotone::ui::layout_channel_mask(target_.layout.channels);
    const isotone::ChannelMask m = static_cast<isotone::ChannelMask>(mask) & all;
    if (mask != 0 && m == 0) return;   // none of the output's speakers
    // Masks address the output's layout.
    state_.layout_channels = target_.layout.channels;
    state_.layout_speaker_mask = target_.layout.speaker_mask;
    state_.bands[order_[static_cast<size_t>(row)]].channels = m == all ? isotone::kAllChannels : m;
    bandChanged(row, {TargetRole, ChannelsRole, ChannelMaskRole});
    commit();
}

void EqSession::setUserGroups(std::vector<isotone::ui::SpeakerGroup> groups) {
    user_groups_ = std::move(groups);
    if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {TargetRole});
}

void EqSession::editSpeakers(const std::function<void(isotone::EqState*)>& edit) {
    state_.layout_channels = target_.layout.channels;
    state_.layout_speaker_mask = target_.layout.speaker_mask;
    edit(&state_);
    updateAutoPreamp();
    // A speaker setup change is saved (engine contract "Saved state"): the file
    // first, then the region (commit).
    saveSpeakerSetup();
    commit();
    emit stateChanged();
    emit curveChanged();
}

void EqSession::saveSpeakerSetup() {
    // The file only, with the saved bands kept.
    if (target_.backend != isotone::ui::Backend::native) return;
    const DWORD error = isotone::ui::save_speaker_setup(link_->saved_state_path(), savedState());
    if (error != ERROR_SUCCESS) emit speakerSaveFailed(static_cast<int>(error));
}

void EqSession::setUndoExtra(double value, bool loaded) {
    extra_ = value;
    if (loaded) baseline_.extra = value;
}

// ---------------------------------------------------------------------------
// Undo and redo, and a preset's EQ

namespace {

bool same_band(const isotone::Band& a, const isotone::Band& b) {
    return a.id == b.id && a.type == b.type && a.fc == b.fc && a.gain_db == b.gain_db && a.width == b.width &&
           a.width_mode == b.width_mode && a.shelf_corner == b.shelf_corner && a.channels == b.channels &&
           a.enabled == b.enabled;
}

// The part a speaker change saves: the layout, levels and speaker setup.
bool same_speaker_part(const isotone::EqState& a, const isotone::EqState& b) {
    if (a.layout_channels != b.layout_channels || a.layout_speaker_mask != b.layout_speaker_mask) return false;
    const isotone::SpeakerSetup& x = a.speakers;
    const isotone::SpeakerSetup& y = b.speakers;
    for (uint32_t c = 0; c < isotone::kMaxChannels; ++c) {
        if (a.channel_gain_db[c] != b.channel_gain_db[c] || x.delay_ms[c] != y.delay_ms[c]) return false;
    }
    return x.inverted == y.inverted && x.muted == y.muted && x.lip_sync_ms == y.lip_sync_ms &&
           x.swap_left_right == y.swap_left_right && x.swap_front_rear == y.swap_front_rear && x.upmix == y.upmix &&
           x.bass_management == y.bass_management && x.crossover_hz == y.crossover_hz &&
           x.small_speakers == y.small_speakers && x.lfe_lowpass_hz == y.lfe_lowpass_hz;
}

bool same_state(const isotone::EqState& a, const isotone::EqState& b) {
    if (a.bypass != b.bypass || a.preamp_db != b.preamp_db || a.auto_preamp != b.auto_preamp || a.mute != b.mute ||
        a.bands.size() != b.bands.size()) {
        return false;
    }
    for (size_t i = 0; i < a.bands.size(); ++i)
        if (!same_band(a.bands[i], b.bands[i])) return false;
    return same_speaker_part(a, b);
}

// The history a session keeps.
constexpr size_t kMaxSteps = 500;

}  // namespace

bool EqSession::record() {
    const uint32_t selected_before = baseline_selected_;
    baseline_selected_ = selected_id_;
    if (balance_ == baseline_.balance && extra_ == baseline_.extra && same_state(state_, baseline_.state)) return false;
    Step step;
    step.before = baseline_;
    step.after = Snapshot{state_, balance_, extra_};
    step.selected_before = selected_before;
    step.selected_after = selected_id_;
    step.id = next_step_++;
    undo_.push_back(std::move(step));
    if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    redo_.clear();
    baseline_ = Snapshot{state_, balance_, extra_};
    emit historyChanged();
    return true;
}

void EqSession::resetHistory() {
    undo_.clear();
    redo_.clear();
    baseline_ = Snapshot{state_, balance_, extra_};
    baseline_selected_ = selected_id_;
    emit historyChanged();
}

void EqSession::restore(const Snapshot& s, uint32_t selected) {
    const bool speakers_changed = !same_speaker_part(state_, s.state);
    beginResetModel();
    state_ = s.state;
    balance_ = s.balance;
    extra_ = s.extra;
    const bool present = std::any_of(state_.bands.begin(), state_.bands.end(),
                                     [&](const isotone::Band& b) { return b.id == selected; });
    selected_id_ = present ? selected : state_.bands.empty() ? 0 : state_.bands.front().id;
    rebuildOrder();
    endResetModel();
    baseline_ = Snapshot{state_, balance_, extra_};
    baseline_selected_ = selected_id_;
    if (speakers_changed) saveSpeakerSetup();   // as the change undone or redone was saved, the file first
    write();
    emit countChanged();
    emit selectionChanged();
    emit stateChanged();
    emit curveChanged();
    emit historyChanged();
    emit committed();
}

void EqSession::undo() {
    record();   // an edit not yet committed is a step of its own
    if (undo_.empty()) return;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    const Snapshot before = step.before;
    const uint32_t selected = step.selected_before;
    redo_.push_back(std::move(step));
    restore(before, selected);
}

void EqSession::redo() {
    if (redo_.empty()) return;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    const Snapshot after = step.after;
    const uint32_t selected = step.selected_after;
    undo_.push_back(std::move(step));
    restore(after, selected);
}

bool EqSession::undoStep(int step) {
    if (undo_.empty() || undo_.back().id != step) return false;
    undo();
    return true;
}

isotone::EqState EqSession::eqPart() const {
    isotone::EqState eq;
    eq.bands = state_.bands;
    eq.preamp_db = state_.preamp_db;
    eq.auto_preamp = state_.auto_preamp;
    eq.layout_channels = target_.layout.channels;
    eq.layout_speaker_mask = target_.layout.speaker_mask;
    return eq;
}

void EqSession::replaceEq(const isotone::EqState& eq) {
    isotone::EqState moved = eq;
    limitBands(&moved.bands);
    isotone::remap_channels(&moved, isotone::ChannelLayout{target_.layout.channels, target_.layout.speaker_mask});
    beginResetModel();
    state_.bands = std::move(moved.bands);
    std::set<uint32_t> seen;
    uint32_t next = 1;
    for (const isotone::Band& b : state_.bands) next = std::max(next, b.id + 1);
    for (isotone::Band& b : state_.bands) {
        if (b.id == 0 || !seen.insert(b.id).second) {
            b.id = next++;
            seen.insert(b.id);
        }
    }
    state_.preamp_db = eq.preamp_db;
    state_.auto_preamp = eq.auto_preamp;
    updateAutoPreamp();
    selected_id_ = state_.bands.empty() ? 0 : state_.bands.front().id;
    rebuildOrder();
    endResetModel();
    emit countChanged();
    emit selectionChanged();
    emit stateChanged();
    emit curveChanged();
}

void EqSession::setLiveOverrides(const isotone::ui::LiveOverrides& overrides) {
    if (overrides.solo_muted == overrides_.solo_muted && overrides.test_tones == overrides_.test_tones) return;
    overrides_ = overrides;
    commit();
}

isotone::EqState EqSession::savedState() const { return isotone::ui::state_for_output(state_, balance_, target_.layout); }

isotone::EqState EqSession::engineState() const {
    isotone::EqState s = savedState();
    isotone::ui::apply_live_overrides(overrides_, &s);
    return s;
}

void EqSession::setEqPart(const isotone::EqState& eq) {
    replaceEq(eq);
    commit();
}

void EqSession::adoptEqPart(const isotone::EqState& eq) {
    replaceEq(eq);
    resetHistory();
}

void EqSession::saveToOutput() {
    switch (target_.backend) {
        case isotone::ui::Backend::native:
            link_->save(savedState(), engineState());
            break;
        case isotone::ui::Backend::equalizer_apo:
            write();   // Isotone.txt is what it starts with
            break;
        case isotone::ui::Backend::none:
            break;
    }
}
