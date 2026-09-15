// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "eqsession.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#include "isotone/param_block.h"
#include "isotone/processor.h"
#include "isotone/response.h"
#include "output_state.h"
#include "outputs.h"

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

}  // namespace

EqSession::EqSession(QObject* parent)
    : QAbstractListModel(parent), link_(std::make_unique<isotone::ui::DeviceLink>()),
      audio_(size_t{8192} * isotone::kMaxChannels) {
    state_.auto_preamp = true;
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
        case TargetRole: return QStringLiteral("L+R");
        case ColorIndexRole: return static_cast<int>((b->id - 1) % kBandColours);
        case SelectedRole: return b->id == selected_id_;
        case WidthLabelRole:
            switch (b->width_mode) {
                case isotone::WidthMode::Q: return QStringLiteral("Q %1").arg(b->width, 0, 'f', 2);
                case isotone::WidthMode::BandwidthOct: return QStringLiteral("%1 oct").arg(b->width, 0, 'f', 2);
                case isotone::WidthMode::SlopeDb: return QStringLiteral("%1 dB/oct").arg(b->width, 0, 'f', 1);
            }
            return {};
    }
    return {};
}

QHash<int, QByteArray> EqSession::roleNames() const {
    return {{BandIdRole, "bandId"},      {PositionRole, "position"}, {TypeNameRole, "typeName"},
            {FrequencyRole, "frequency"}, {GainRole, "gain"},         {QRole, "q"},
            {EnabledRole, "bandEnabled"}, {TargetRole, "target"},     {ColorIndexRole, "colorIndex"},
            {SelectedRole, "selected"},   {WidthLabelRole, "widthLabel"}};
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
    const bool same = target.guid == target_.guid && target.backend == target_.backend;
    target_ = target;
    link_->set_target(target_);
    link_->take_region_opened();   // what is loaded below is what the region holds
    analyzer_.reset();
    if (same) return;   // a format change on the same output keeps what is being edited

    // Start from what the output plays.
    isotone::EqState loaded;
    loadState(link_->load_current(&loaded) ? &loaded : nullptr);
}

void EqSession::loadState(const isotone::EqState* state) {
    beginResetModel();
    if (state) {
        isotone::EqState loaded = *state;
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
    } else {
        state_ = isotone::EqState{};
        state_.auto_preamp = true;
        balance_ = 0.0;
    }
    selected_id_ = state_.bands.empty() ? 0 : state_.bands.front().id;
    rebuildOrder();
    endResetModel();
    emit countChanged();
    emit selectionChanged();
    emit stateChanged();
    emit curveChanged();
}

void EqSession::push() {
    if (target_.backend == isotone::ui::Backend::none) return;
    link_->apply(isotone::ui::state_for_output(state_, balance_, target_.layout));
}

void EqSession::commit() {
    if (target_.backend == isotone::ui::Backend::none) return;
    link_->commit(isotone::ui::state_for_output(state_, balance_, target_.layout));
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
    if (link_->take_region_opened()) commit();
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

void EqSession::bandChanged(int row, const QList<int>& roles) {
    emit dataChanged(index(row), index(row), roles);
    updateAutoPreamp();
    push();
    emit curveChanged();
}

void EqSession::updateAutoPreamp() {
    if (!state_.auto_preamp) return;
    static const std::vector<double> grid = isotone::log_grid(20.0, 20000.0, 512);
    const double rate = target_.layout.sample_rate > 0 ? target_.layout.sample_rate : 48000.0;
    const double db = isotone::auto_preamp_db(state_, target_.layout.channels, target_.layout.speaker_mask, grid.data(),
                                              grid.size(), rate);
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
    if (previous >= 0) emit dataChanged(index(previous), index(previous), {SelectedRole});
    emit dataChanged(index(row), index(row), {SelectedRole});
    emit selectionChanged();
    emit curveChanged();
}

void EqSession::setGain(int row, double db) {
    if (bandAt(row) == nullptr || !std::isfinite(db)) return;
    state_.bands[order_[static_cast<size_t>(row)]].gain_db = std::clamp(db, -24.0, 24.0);
    bandChanged(row, {GainRole});
}

void EqSession::setFrequency(int row, double hz) {
    if (bandAt(row) == nullptr || !std::isfinite(hz)) return;
    state_.bands[order_[static_cast<size_t>(row)]].fc = std::clamp(hz, 10.0, 22000.0);
    bandChanged(row, {FrequencyRole});
}

void EqSession::setWidth(int row, double width) {
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
    commit();   // a wheel step is a whole edit
}

void EqSession::setEnabled(int row, bool on) {
    if (bandAt(row) == nullptr) return;
    state_.bands[order_[static_cast<size_t>(row)]].enabled = on;
    bandChanged(row, {EnabledRole});
    commit();
}

bool EqSession::canAddBand() const { return state_.bands.size() < isotone::kParamMaxBands; }

void EqSession::addBand(double hz, double db) {
    if (!canAddBand() || !std::isfinite(hz) || !std::isfinite(db)) return;
    uint32_t id = 1;
    for (const isotone::Band& b : state_.bands) id = std::max(id, b.id + 1);
    isotone::Band b;
    b.id = id;
    b.type = isotone::FilterType::Peaking;
    b.fc = std::clamp(hz, 10.0, 22000.0);
    b.gain_db = std::clamp(std::round(db * 10.0) / 10.0, -24.0, 24.0);
    b.width = 1.41;
    beginResetModel();
    state_.bands.push_back(b);
    selected_id_ = id;
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
