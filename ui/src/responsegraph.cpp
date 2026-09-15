// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "responsegraph.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

#include "eqsession.h"
#include "isotone/response.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kFMin = 20.0, kFMax = 20000.0;
const double kDecades = std::log(kFMax / kFMin);

QString minus_sign(QString s) { return s.replace(QLatin1Char('-'), QChar(0x2212)); }

}  // namespace

ResponseGraph::ResponseGraph(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &ResponseGraph::styleChanged, this, [this] { update(); });
}

void ResponseGraph::setSession(EqSession* s) {
    if (session_ == s) return;
    if (session_) disconnect(session_, nullptr, this, nullptr);
    session_ = s;
    if (session_) {
        connect(session_, &EqSession::curveChanged, this, &ResponseGraph::curveChanged);
        connect(session_, &EqSession::spectrumChanged, this, [this] { update(); });
    }
    emit sessionChanged();
    curveChanged();
}

void ResponseGraph::curveChanged() {
    ++revision_;
    emit revisionChanged();
    update();
}

void ResponseGraph::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickPaintedItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        emit geometryChanged();
        curveChanged();
    }
}

double ResponseGraph::xOf(double hz) const { return kLeft + plotWidth() * std::log(hz / kFMin) / kDecades; }

double ResponseGraph::yOf(double db) const { return kTop + plotHeight() / 2.0 - db * (plotHeight() / 2.0) / range_db_; }

double ResponseGraph::frequencyAt(double x) const {
    const double t = std::clamp((x - kLeft) / plotWidth(), 0.0, 1.0);
    return kFMin * std::exp(t * kDecades);
}

double ResponseGraph::dbAt(double y) const { return (kTop + plotHeight() / 2.0 - y) * range_db_ / (plotHeight() / 2.0); }

uint32_t ResponseGraph::viewChannel() const {
    const bool stereo = session_ && session_->layout().channels == 2;
    return stereo && session_->viewChannel() == 1 ? 1 : 0;
}

double ResponseGraph::compositeOn(uint32_t channel, double hz) const {
    if (!session_) return 0.0;
    isotone::EqState s = session_->state();
    s.preamp_db = 0.0;
    s.bypass = false;
    s.mute = false;
    double out = 0.0;
    isotone::magnitude_db(s, session_->layout().channels, session_->layout().speaker_mask, channel, &hz, 1, kSampleRate,
                          &out);
    return out;
}

double ResponseGraph::compositeAt(double hz) const { return compositeOn(viewChannel(), hz); }

double ResponseGraph::handleDb(int row) const {
    const isotone::Band* b = session_ ? session_->bandAt(row) : nullptr;
    if (b == nullptr) return 0.0;
    const bool both = session_->layout().channels == 2 && session_->viewChannel() == 2;
    const bool right_only = (b->channels & 0x3) == 0x2;
    return compositeOn(both && right_only ? 1 : viewChannel(), b->fc);
}

bool ResponseGraph::onView(int row) const {
    const isotone::Band* b = session_ ? session_->bandAt(row) : nullptr;
    if (b == nullptr) return false;
    if (session_->layout().channels != 2 || session_->viewChannel() == 2) return true;
    return isotone::band_affects_channel(*b, viewChannel());
}

void ResponseGraph::paint(QPainter* p) {
    const double h = height();
    const double pw = plotWidth(), ph = plotHeight();
    if (pw <= 0 || ph <= 0) return;
    p->setRenderHint(QPainter::Antialiasing, true);

    // Grid: every 1-2-5 decade line major, the rest minor.
    const double grid_lines[] = {20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800,
                                 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000};
    const auto major = [](double f) {
        for (double m : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0})
            if (f == m) return true;
        return false;
    };
    for (double f : grid_lines) {
        p->setPen(QPen(major(f) ? grid_major_ : grid_minor_, 1.0));
        const double x = std::round(xOf(f)) + 0.5;
        p->drawLine(QPointF(x, kTop), QPointF(x, kTop + ph));
    }

    QFont font(font_family_);
    font.setPixelSize(11);
    font.setFeature(QFont::Tag("tnum"), 1);
    p->setFont(font);
    const QFontMetricsF fm(font);
    p->setPen(label_colour_);
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        const QString label = f >= 1000 ? QStringLiteral("%1k").arg(f / 1000) : QString::number(f);
        const double tw = fm.horizontalAdvance(label);
        const double x = f == 20 ? xOf(f) : f == 20000 ? xOf(f) - tw : xOf(f) - tw / 2;
        p->drawText(QPointF(x, h - 10), label);
    }
    const double step = range_db_ >= 20 ? 12.0 : 6.0;
    for (double level = -std::floor(range_db_ / step) * step; level <= range_db_; level += step) {
        const double y = std::round(yOf(level)) + 0.5;
        p->setPen(QPen(level == 0 ? zero_line_ : grid_major_, 1.0));
        p->drawLine(QPointF(kLeft, y), QPointF(kLeft + pw, y));
        const QString label =
            level == 0 ? QStringLiteral("0") : minus_sign(QString::asprintf("%+d", static_cast<int>(level)));
        p->setPen(label_colour_);
        p->drawText(QPointF(kLeft - 10 - fm.horizontalAdvance(label), yOf(level) + 4), label);
    }

    const size_t n = static_cast<size_t>(std::max(64.0, pw));
    std::vector<double> freqs(n);
    for (size_t i = 0; i < n; ++i) freqs[i] = kFMin * std::exp(kDecades * static_cast<double>(i) / static_cast<double>(n - 1));
    const auto x_at = [&](size_t i) { return kLeft + pw * static_cast<double>(i) / static_cast<double>(n - 1); };

    const bool muted = session_ && session_->state().mute;

    // Spectrum on its own scale: 0 dBFS at the top of the plot, -90 at the bottom.
    // None while no audio arrives (the engine is idle).
    std::vector<double> spectrum(n);
    if (spectrum_visible_ && !muted && session_ && session_->spectrumLevels(freqs.data(), n, spectrum.data())) {
        QPainterPath edge;
        for (size_t i = 0; i < n; ++i) {
            const double v = std::min(0.0, spectrum[i]);
            const QPointF pt(x_at(i), std::min(kTop + ph, kTop + ph * (-v / 90.0)));
            i == 0 ? edge.moveTo(pt) : edge.lineTo(pt);
        }
        QPainterPath area = edge;
        area.lineTo(kLeft + pw, kTop + ph);
        area.lineTo(kLeft, kTop + ph);
        area.closeSubpath();
        p->fillPath(area, spectrum_fill_);
        p->strokePath(edge, QPen(spectrum_edge_, 1.0));
    }
    if (!session_) return;

    const isotone::EqState& state = session_->state();
    const auto polyline = [&](const std::vector<double>& db) {
        QPainterPath path;
        for (size_t i = 0; i < n; ++i) {
            const QPointF pt(x_at(i), std::clamp(yOf(db[i]), kTop - 2.0, kTop + ph + 2.0));
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        return path;
    };

    // Bells, each band alone.
    std::vector<double> db(n);
    if (!state.bypass && !muted) {
        for (int row = 0; row < session_->rowCount(); ++row) {
            const isotone::Band* b = session_->bandAt(row);
            if (!b->enabled || !onView(row)) continue;
            isotone::band_magnitude_db(*b, freqs.data(), n, kSampleRate, db.data());
            QColor c = bell_;
            if (per_band_ && !band_colours_.isEmpty()) {
                c = band_colours_[static_cast<int>((b->id - 1) % static_cast<uint32_t>(band_colours_.size()))].value<QColor>();
                c.setAlphaF(0.5f);
            }
            p->strokePath(polyline(db), QPen(c, 1.25));
        }
    }

    // Composite, without preamp; flat under bypass, nothing when muted.
    if (muted) return;
    isotone::EqState drawn = state;
    drawn.preamp_db = 0.0;
    const uint32_t channels = session_->layout().channels, mask = session_->layout().speaker_mask;
    isotone::magnitude_db(drawn, channels, mask, viewChannel(), freqs.data(), n, kSampleRate, db.data());
    const QPainterPath curve = polyline(db);

    if (!state.bypass) {
        QPainterPath fill = curve;
        fill.lineTo(kLeft + pw, yOf(0));
        fill.lineTo(kLeft, yOf(0));
        fill.closeSubpath();
        QLinearGradient g(0, kTop, 0, kTop + ph);
        QColor edge = accent_, mid = accent_;
        edge.setAlphaF(static_cast<float>(fill_edge_alpha_));
        mid.setAlphaF(static_cast<float>(fill_mid_alpha_));
        g.setColorAt(0.0, edge);
        g.setColorAt(0.5, mid);
        g.setColorAt(1.0, edge);
        p->fillPath(fill, g);
    }
    // L+R with channels that differ: the right channel as a second line.
    if (channels == 2 && session_->viewChannel() == 2 && !state.bypass) {
        std::vector<double> right(n);
        isotone::magnitude_db(drawn, channels, mask, 1, freqs.data(), n, kSampleRate, right.data());
        bool differs = false;
        for (size_t i = 0; i < n && !differs; ++i) differs = std::abs(right[i] - db[i]) > 0.01;
        if (differs) {
            // As the prototype draws it: the right dashed, and L and R marked at the right edge.
            QPen right_pen(accent_, 2.5);
            right_pen.setJoinStyle(Qt::RoundJoin);
            right_pen.setCapStyle(Qt::FlatCap);
            right_pen.setDashPattern({7.0 / 2.5, 5.0 / 2.5});
            p->strokePath(polyline(right), right_pen);
            QFont bold(font_family_);
            bold.setPixelSize(11);
            bold.setWeight(QFont::DemiBold);
            p->setFont(bold);
            p->setPen(accent_);
            const double lx = kLeft + pw - 14;
            p->drawText(QPointF(lx, std::clamp(yOf(db[n - 1]) - 8, kTop + 10, kTop + ph)), QStringLiteral("L"));
            p->drawText(QPointF(lx, std::clamp(yOf(right[n - 1]) + 16, kTop + 10, kTop + ph)), QStringLiteral("R"));
        }
    }
    QPen pen(accent_, 2.5);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p->strokePath(curve, pen);
}
