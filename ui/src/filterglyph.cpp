// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "filterglyph.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

#include "isotone/response.h"

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kPoints = 160;

struct Tile {
    double gain_db;
    double q;
};

// Indexed by isotone::FilterType. Every tile's fc is the middle of the log axis.
constexpr Tile kTiles[] = {
    {11.0, 1.6},   // Peaking
    {0.0, 0.8},    // LowPass
    {0.0, 0.8},    // HighPass
    {0.0, 1.6},    // BandPass
    {0.0, 2.5},    // Notch
    {0.0, 0.9},    // AllPass
    {10.0, 0.7},   // LowShelf
    {10.0, 0.7},   // HighShelf
};

}  // namespace

FilterGlyph::FilterGlyph(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &FilterGlyph::changed, this, [this] { update(); });
}

void FilterGlyph::paint(QPainter* p) {
    if (type_ < 0 || type_ > static_cast<int>(isotone::FilterType::HighShelf)) return;
    const double w = width(), h = height();
    p->setRenderHint(QPainter::Antialiasing, true);
    QPainterPath box;
    box.addRoundedRect(QRectF(0, 0, w, h), 7, 7);
    p->fillPath(box, background_);

    isotone::EqState s;
    isotone::Band b;
    b.id = 1;
    b.type = static_cast<isotone::FilterType>(type_);
    b.fc = std::sqrt(20.0 * 20000.0);
    b.gain_db = kTiles[type_].gain_db;
    b.width = kTiles[type_].q;
    s.bands.push_back(b);

    const std::vector<double> freqs = isotone::log_grid(20.0, 20000.0, kPoints);
    const auto x_of = [&](double f) { return 4.0 + (w - 8.0) * std::log(f / 20.0) / std::log(1000.0); };
    const double mid = h / 2.0, span = h / 2.0 - 4.0;
    std::vector<double> v(kPoints);

    QPen line(stroke_, 2.0);
    line.setCapStyle(Qt::RoundCap);
    line.setJoinStyle(Qt::RoundJoin);

    if (b.type == isotone::FilterType::AllPass) {
        isotone::phase_deg(s, 1, 0, 0, freqs.data(), kPoints, kRate, v.data());
        for (size_t i = 1; i < kPoints; ++i) {   // unwrap
            while (v[i] - v[i - 1] > 180.0) v[i] -= 360.0;
            while (v[i] - v[i - 1] < -180.0) v[i] += 360.0;
        }
        QPainterPath phase;
        for (size_t i = 0; i < kPoints; ++i) {
            const QPointF pt(x_of(freqs[i]), mid - span + 2.0 * span * (-v[i] / 360.0));
            i == 0 ? phase.moveTo(pt) : phase.lineTo(pt);
        }
        p->setPen(line);
        p->drawLine(QPointF(4, mid), QPointF(w - 4, mid));
        QColor faint = stroke_;
        faint.setAlphaF(0.45f);
        QPen phase_pen = line;
        phase_pen.setColor(faint);
        phase_pen.setWidthF(1.5);
        p->strokePath(phase, phase_pen);
        return;
    }

    isotone::band_magnitude_db(b, freqs.data(), kPoints, kRate, v.data());
    for (double& level : v) level = std::clamp(std::isfinite(level) ? level : -13.0, -13.0, 13.0);
    // The shape's extent is centred in the box.
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    const double shift = (*lo + *hi) / 2.0;
    const auto y_of = [&](double level) { return mid - (level - shift) * span / 13.0; };

    p->setPen(QPen(zero_line_, 1.0));
    p->drawLine(QPointF(4, y_of(0)), QPointF(w - 4, y_of(0)));
    QPainterPath curve;
    for (size_t i = 0; i < kPoints; ++i) {
        const QPointF pt(x_of(freqs[i]), y_of(v[i]));
        i == 0 ? curve.moveTo(pt) : curve.lineTo(pt);
    }
    QPainterPath fill = curve;
    fill.lineTo(x_of(20000.0), y_of(0));
    fill.lineTo(x_of(20.0), y_of(0));
    fill.closeSubpath();
    QColor tint = stroke_;
    tint.setAlphaF(selected_ ? 0.22f : 0.12f);
    p->fillPath(fill, tint);
    p->strokePath(curve, line);
}
