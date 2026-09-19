// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "eqbyear.h"
#include "qmlsingleton.h"

#include <QQmlEngine>

#include <algorithm>
#include <cmath>

#include "eqsession.h"
#include "speaker_setup.h"

namespace {

// Channel changes restart the stream on the new channels once the fade out has
// played: the fade, and the audio already queued (about 50 ms).
constexpr int kChannelSwitchMs = 120;

}  // namespace

EqByEar::MarkedBand EqByEar::bandFromMarks(double start, double top, double end, bool dip) {
    const double span = std::abs(end - start);
    const double q = span > 0.0 ? std::clamp(top / span, 0.1, 50.0) : 50.0;
    return MarkedBand{top, dip ? 3.0 : -3.0, std::round(q * 100.0) / 100.0};
}

EqByEar* EqByEar::create(QQmlEngine* qml, QJSEngine*) {
    return new EqByEar(isotoneSingleton<EqSession>(qml, "EqSession"));
}

EqByEar::EqByEar(EqSession* session, QObject* parent) : QObject(parent), session_(session) {
    sine_.set_frequency(frequency_);
    sine_.set_level_db(level_db_);
    follow_.setInterval(33);
    connect(&follow_, &QTimer::timeout, this, [this] {
        const double hz = sine_.frequency();
        if (hz == frequency_) return;
        frequency_ = hz;
        emit frequencyChanged();
    });
    idle_.setSingleShot(true);
    idle_.setInterval(kIdleStreamMs);
    connect(&idle_, &QTimer::timeout, this, &EqByEar::stopStream);
    if (session_) {
        seen_ = session_->target();
        supported_ = supported();
        connect(session_, &EqSession::stateChanged, this, &EqByEar::sessionChanged);
        connect(session_, &EqSession::targetChanged, this, &EqByEar::sessionChanged);
        connect(session_, &EqSession::viewChanged, this, &EqByEar::followView);
        followView();
    }
}

EqByEar::~EqByEar() { tone_.stop(); }

bool EqByEar::supported() const {
    if (!session_) return false;
    const isotone::ui::OutputLayout& l = session_->target().layout;
    return l.channels == 2 || (l.channels == 3 && l.speaker_mask == 0xB);
}

void EqByEar::sessionChanged() {
    const isotone::ui::OutputTarget& now = session_->target();
    if (now.guid == seen_.guid && now.backend == seen_.backend && now.layout.channels == seen_.layout.channels &&
        now.layout.speaker_mask == seen_.layout.speaker_mask) {
        return;
    }
    seen_ = now;
    // Another output, or this one in another layout: its tone and marks are gone.
    setPlaying(false);
    stopStream();
    clearMarks();
    followView();
    if (supported() != supported_) {
        supported_ = supported();
        emit supportedChanged();
    }
}

void EqByEar::followView() {
    int channel = Both;
    if (session_->target().layout.channels == 2) {
        channel = session_->viewChannel() == 0 ? Left : session_->viewChannel() == 1 ? Right : Both;
    } else {
        const int front = session_->showingMask() & 0x3;   // FL and FR, channels 0 and 1
        channel = front == 0x1 ? Left : front == 0x2 ? Right : Both;
    }
    setChannel(channel);
}

void EqByEar::setPlaying(bool on) {
    if (on && !supported()) return;
    if (on == playing_) return;
    playing_ = on;
    sine_.set_on(on);
    applySweep();
    if (on) {
        idle_.stop();
        if (!stream_ || stream_mask_ != toneMask()) startStream();
    } else {
        idle_.start();
    }
    emit playingChanged();
}

void EqByEar::stop() { setPlaying(false); }

void EqByEar::setFrequency(double hz) {
    if (!std::isfinite(hz)) return;
    hz = std::clamp(hz, isotone::ui::kEarToneLowHz, isotone::ui::kEarToneHighHz);
    sine_.set_frequency(hz);
    if (hz == frequency_) return;
    frequency_ = hz;
    emit frequencyChanged();
}

void EqByEar::nudge(double octaves) { setFrequency(frequency_ * std::pow(2.0, octaves)); }

void EqByEar::setLevelDb(double db) {
    if (!std::isfinite(db)) return;
    db = std::clamp(db, kLevelMinDb, 0.0);
    if (db == level_db_) return;
    level_db_ = db;
    sine_.set_level_db(db);
    emit levelChanged();
}

void EqByEar::setChannel(int channel) {
    if (channel < Left || channel > Both || channel == channel_) return;
    channel_ = channel;
    if (stream_ && playing_) {
        // Faded out, then on the new channels.
        sine_.set_on(false);
        const quint64 generation = generation_;
        QTimer::singleShot(kChannelSwitchMs, this, [this, generation] {
            if (generation != generation_ || !playing_) return;
            startStream();
            sine_.set_on(true);
        });
    } else if (stream_) {
        stopStream();
    }
    emit channelChanged();
}

void EqByEar::setAutoSweep(bool on) {
    if (on == auto_sweep_) return;
    auto_sweep_ = on;
    applySweep();
    emit sweepChanged();
}

void EqByEar::setSweepRate(double octaves_per_second) {
    if (!std::isfinite(octaves_per_second)) return;
    octaves_per_second = std::clamp(octaves_per_second, kSweepRateMin, kSweepRateMax);
    if (octaves_per_second == sweep_rate_) return;
    sweep_rate_ = octaves_per_second;
    applySweep();
    emit sweepChanged();
}

void EqByEar::applySweep() {
    const bool sweeping = playing_ && auto_sweep_;
    sine_.set_sweep(sweeping ? sweep_rate_ : 0.0);
    if (sweeping) {
        follow_.start();
    } else if (follow_.isActive()) {
        follow_.stop();
        // It stays where the sweep stopped it.
        if (stream_) setFrequency(sine_.frequency());
    }
}

void EqByEar::setDip(bool on) {
    if (on == dip_) return;
    dip_ = on;
    emit marksChanged();
}

void EqByEar::mark(int which) {
    if (which < Start || which > End) return;
    marks_[which] = std::round(frequency_);
    emit marksChanged();
}

void EqByEar::clearMarks() {
    if (marks_[Start] == 0.0 && marks_[Top] == 0.0 && marks_[End] == 0.0) return;
    marks_[Start] = marks_[Top] = marks_[End] = 0.0;
    emit marksChanged();
}

bool EqByEar::canAddBand() const {
    if (!session_ || marks_[Start] <= 0.0 || marks_[Top] <= 0.0 || marks_[End] <= 0.0) return false;
    // At the band limit, only onto a peak that is already there.
    return session_->canAddBand() || peakAt(marks_[Top], bandMask()) >= 0;
}

int EqByEar::bandMask() const {
    // Stereo: one channel, or every channel. 2.1: one of the front pair, or both.
    const bool two_one = session_ && session_->target().layout.channels == 3;
    return channel_ == Left ? 0x1 : channel_ == Right ? 0x2 : two_one ? 0x3 : 0;
}

void EqByEar::addBand() {
    if (!canAddBand()) return;
    const MarkedBand b = bandFromMarks(marks_[Start], marks_[Top], marks_[End], dip_);
    const int mask = bandMask();
    const int existing = peakAt(b.fc, mask);
    if (existing >= 0) {
        // Heard again where a peak already is: that peak goes further (owner,
        // 2026-09-16), where it is and as wide as it is.
        session_->select(existing);
        session_->setGain(existing, session_->bandAt(existing)->gain_db + b.gainDb);
        session_->finishEdit();
    } else {
        session_->addBand(b.fc, b.gainDb, b.q, mask);
    }
    marks_[Start] = marks_[Top] = marks_[End] = 0.0;
    emit marksChanged();
}

int EqByEar::peakAt(double hz, int channelMask) const {
    const isotone::ChannelMask all = isotone::ui::layout_channel_mask(session_->target().layout.channels);
    const auto on = [&](isotone::ChannelMask m) { return m == isotone::kAllChannels ? all : m & all; };
    int nearest = -1;
    double best = kSamePeakOctaves;
    for (int row = 0; row < session_->rowCount(); ++row) {
        const isotone::Band* b = session_->bandAt(row);
        if (!b->enabled || b->type != isotone::FilterType::Peaking ||
            on(b->channels) != on(static_cast<isotone::ChannelMask>(channelMask))) {
            continue;
        }
        const double distance = std::abs(std::log2(b->fc / hz));
        if (distance <= best) {
            best = distance;
            nearest = row;
        }
    }
    return nearest;
}

uint32_t EqByEar::toneMask() const { return channel_ == Left ? 0x1u : channel_ == Right ? 0x2u : 0x3u; }

void EqByEar::startStream() {
    // No output behind the session (a test): nothing to play on.
    if (!session_ || session_->target().backend == isotone::ui::Backend::none || session_->target().guid.empty()) return;
    const quint64 generation = ++generation_;
    stream_ = true;
    stream_guid_ = session_->target().guid;
    stream_mask_ = toneMask();
    QPointer<EqByEar> self(this);
    tone_.start(
        stream_guid_, stream_mask_,
        [this](float* out, uint32_t frames, double sample_rate) { sine_.render(out, frames, sample_rate); },
        [self, generation](int32_t hr, const char*) {
            QMetaObject::invokeMethod(
                self.data(),
                [self, hr, generation] {
                    // A stream stopped since is not the one that failed.
                    if (!self || self->generation_ != generation) return;
                    self->stopStream();
                    self->setPlaying(false);
                    emit self->toneFailed(static_cast<int>(hr));
                },
                Qt::QueuedConnection);
        });
}

void EqByEar::stopStream() {
    idle_.stop();
    tone_.stop();
    ++generation_;
    stream_ = false;
}
