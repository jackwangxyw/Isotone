// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "curvepreview.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

#include "isotone/response.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kFMin = 20.0, kFMax = 20000.0;
constexpr double kRangeDb = 15.0;
// ResponseGraph's margins.
constexpr double kLeft = 46.0, kRight = 14.0, kTop = 14.0, kBottom = 30.0;

QString minus_sign(QString s) { return s.replace(QLatin1Char('-'), QChar(0x2212)); }

}  // namespace

CurvePreview::CurvePreview(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &CurvePreview::styleChanged, this, [this] { update(); });
}

void CurvePreview::setPreview(ImportPreview* p) {
    if (preview_ == p) return;
    if (preview_) disconnect(preview_, nullptr, this, nullptr);
    preview_ = p;
    if (preview_) connect(preview_, &ImportPreview::changed, this, [this] { update(); });
    emit previewChanged();
    update();
}

double CurvePreview::compositeAt(double hz) const {
    if (!preview_) return 0.0;
    isotone::EqState s = preview_->state();
    s.preamp_db = 0.0;
    double out = 0.0;
    const uint32_t channels = s.layout_channels ? s.layout_channels : 2;
    isotone::magnitude_db(s, channels, s.layout_speaker_mask, 0, &hz, 1, kSampleRate, &out);
    return out;
}

void CurvePreview::paint(QPainter* p) {
    const double pw = width() - kLeft - kRight, ph = height() - kTop - kBottom;
    if (pw <= 0 || ph <= 0) return;
    p->setRenderHint(QPainter::Antialiasing, true);
    const double decades = std::log(kFMax / kFMin);
    const auto x_of = [&](double hz) { return kLeft + pw * std::log(hz / kFMin) / decades; };
    const auto y_of = [&](double db) { return kTop + ph / 2.0 - db * (ph / 2.0) / kRangeDb; };

    const double grid_lines[] = {20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800,
                                 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000};
    const double majors[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (double f : grid_lines) {
        const bool major = std::find(std::begin(majors), std::end(majors), f) != std::end(majors);
        p->setPen(QPen(major ? grid_major_ : grid_minor_, 1.0));
        const double x = std::round(x_of(f)) + 0.5;
        p->drawLine(QPointF(x, kTop), QPointF(x, kTop + ph));
    }
    QFont font(font_family_);
    font.setPixelSize(11);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    p->setFont(font);
    const QFontMetricsF fm(font);
    for (double f : majors) {
        const QString label = f >= 1000 ? QStringLiteral("%1k").arg(f / 1000) : QString::number(f);
        const double tw = fm.horizontalAdvance(label);
        const double x = f == kFMin ? x_of(f) : f == kFMax ? x_of(f) - tw : x_of(f) - tw / 2;
        p->setPen(label_colour_);
        p->drawText(QPointF(x, height() - 10), label);
    }
    for (double level = -12; level <= 12; level += 6) {
        const double y = std::round(y_of(level)) + 0.5;
        p->setPen(QPen(level == 0 ? zero_line_ : grid_major_, 1.0));
        p->drawLine(QPointF(kLeft, y), QPointF(kLeft + pw, y));
        const QString label = level == 0 ? QStringLiteral("0") : minus_sign(QString::asprintf("%+d", static_cast<int>(level)));
        p->setPen(label_colour_);
        p->drawText(QPointF(kLeft - 10 - fm.horizontalAdvance(label), y_of(level) + 4), label);
    }
    if (!preview_) return;

    const size_t n = static_cast<size_t>(std::max(64.0, pw));
    std::vector<double> freqs(n), db(n);
    for (size_t i = 0; i < n; ++i) freqs[i] = kFMin * std::exp(decades * static_cast<double>(i) / static_cast<double>(n - 1));
    const auto polyline = [&](const std::vector<double>& v) {
        QPainterPath path;
        for (size_t i = 0; i < n; ++i) {
            const QPointF pt(kLeft + pw * static_cast<double>(i) / static_cast<double>(n - 1),
                             std::clamp(y_of(v[i]), kTop - 2.0, kTop + ph + 2.0));
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        return path;
    };

    isotone::EqState s = preview_->state();
    s.preamp_db = 0.0;
    for (const isotone::Band& b : s.bands) {
        if (!b.enabled || !isotone::band_affects_channel(b, 0)) continue;
        isotone::band_magnitude_db(b, freqs.data(), n, kSampleRate, db.data());
        p->strokePath(polyline(db), QPen(bell_, 1.25));
    }
    const uint32_t channels = s.layout_channels ? s.layout_channels : 2;
    isotone::magnitude_db(s, channels, s.layout_speaker_mask, 0, freqs.data(), n, kSampleRate, db.data());
    const QPainterPath curve = polyline(db);
    QPainterPath fill = curve;
    fill.lineTo(kLeft + pw, y_of(0));
    fill.lineTo(kLeft, y_of(0));
    fill.closeSubpath();
    QLinearGradient g(0, kTop, 0, kTop + ph);
    QColor edge = accent_, mid = accent_;
    edge.setAlphaF(static_cast<float>(fill_edge_alpha_));
    mid.setAlphaF(static_cast<float>(fill_mid_alpha_));
    g.setColorAt(0.0, edge);
    g.setColorAt(0.5, mid);
    g.setColorAt(1.0, edge);
    p->fillPath(fill, g);
    QPen pen(accent_, 2.5);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p->strokePath(curve, pen);
}
