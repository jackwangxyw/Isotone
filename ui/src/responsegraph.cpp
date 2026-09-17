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
#include "speaker_setup.h"

// The session decides when the curve has fallen off the plot's bottom; the graph
// draws that same bottom.
static_assert(ResponseGraph::kSpectrumRangeDb == EqSession::kSpectrumRangeDb);

namespace {

constexpr double kSampleRate = 48000.0;

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
        // The grid does not move with the curve.
        connect(session_, &EqSession::curveChanged, this, [this] {
            if (part_ != Grid) curveChanged();
        });
        connect(session_, &EqSession::spectrumChanged, this, [this] {
            if (part_ == All || part_ == Spectrum) update();
        });
    }
    emit sessionChanged();
    curveChanged();
}

void ResponseGraph::setPart(Part part) {
    if (part == part_) return;
    part_ = part;
    emit styleChanged();
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

void ResponseGraph::setRangeDb(double db) {
    if (db == range_db_ || !(db > 0)) return;
    range_db_ = db;
    emit rangeChanged();
    curveChanged();
}

void ResponseGraph::setMinHz(double hz) {
    if (hz == min_hz_ || !(hz > 0)) return;
    min_hz_ = hz;
    emit rangeChanged();
    curveChanged();
}

void ResponseGraph::setMaxHz(double hz) {
    if (hz == max_hz_ || !(hz > 0)) return;
    max_hz_ = hz;
    emit rangeChanged();
    curveChanged();
}

std::vector<ResponseGraph::GridLine> ResponseGraph::gridLines(double min_hz, double max_hz) {
    std::vector<GridLine> lines;
    const double lo = min_hz * (1 - 1e-9), hi = max_hz * (1 + 1e-9);
    for (double decade = std::pow(10.0, std::floor(std::log10(min_hz))); decade <= hi; decade *= 10.0) {
        for (double m : {1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0}) {
            const double f = std::round(m * decade * 1e6) / 1e6;
            if (f >= lo && f <= hi) lines.push_back({f, m == 1.0 || m == 2.0 || m == 5.0});
        }
    }
    if (lines.size() >= 3) return lines;
    // Narrower than that: even steps of the power of ten under the span, major every fifth.
    lines.clear();
    const double step = std::pow(10.0, std::floor(std::log10(max_hz - min_hz)));
    for (double n = std::ceil(lo / step); n * step <= hi; n += 1.0) lines.push_back({n * step, std::fmod(n, 5.0) == 0.0});
    return lines;
}

std::vector<double> ResponseGraph::labelFrequencies(double min_hz, double max_hz) {
    const std::vector<GridLine> lines = gridLines(min_hz, max_hz);
    std::vector<double> major, all;
    for (const GridLine& l : lines) {
        all.push_back(l.hz);
        if (l.major) major.push_back(l.hz);
    }
    if (major.size() >= 3) return major;
    if (all.size() >= 2) return all;
    std::vector<double> ends{min_hz};
    for (double f : all)
        if (f > min_hz && f < max_hz) ends.push_back(f);
    ends.push_back(max_hz);
    return ends;
}

QColor ResponseGraph::separatorColour(const QColor& grid_major, const QColor& zero_line) {
    constexpr float t = 0.25f;
    const auto mix = [](float a, float b) { return a + (b - a) * t; };
    return QColor::fromRgbF(mix(grid_major.redF(), zero_line.redF()), mix(grid_major.greenF(), zero_line.greenF()),
                            mix(grid_major.blueF(), zero_line.blueF()), mix(grid_major.alphaF(), zero_line.alphaF()));
}

bool ResponseGraph::isDecade(double hz) {
    const double exponent = std::log10(hz);
    return std::abs(exponent - std::round(exponent)) < 1e-9;
}

double ResponseGraph::xOf(double hz) const {
    return kLeft + plotWidth() * std::log(hz / min_hz_) / std::log(drawnMaxHz() / min_hz_);
}

double ResponseGraph::yOf(double db) const { return kTop + plotHeight() / 2.0 - db * (plotHeight() / 2.0) / range_db_; }

double ResponseGraph::frequencyAt(double x) const {
    const double t = std::clamp((x - kLeft) / plotWidth(), 0.0, 1.0);
    return min_hz_ * std::exp(t * std::log(drawnMaxHz() / min_hz_));
}

double ResponseGraph::dbAt(double y) const { return (kTop + plotHeight() / 2.0 - y) * range_db_ / (plotHeight() / 2.0); }

uint32_t ResponseGraph::viewChannel() const {
    // Surround: the channel in view with the most bands (speaker_setup.h).
    if (session_ && session_->layout().channels > 2)
        return isotone::ui::primary_view_channel(session_->state(), session_->layout().channels, session_->showingMask());
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
    if (session_->layout().channels > 2)
        return compositeOn(isotone::ui::band_view_channel(*b, session_->layout().channels, session_->showingMask(), viewChannel()), b->fc);
    const bool both = session_->layout().channels == 2 && session_->viewChannel() == 2;
    const bool right_only = (b->channels & 0x3) == 0x2;
    return compositeOn(both && right_only ? 1 : viewChannel(), b->fc);
}

bool ResponseGraph::onView(int row) const {
    const isotone::Band* b = session_ ? session_->bandAt(row) : nullptr;
    if (b == nullptr) return false;
    if (session_->layout().channels > 2)
        return isotone::ui::band_in_view(*b, session_->layout().channels, session_->showingMask());
    if (session_->layout().channels != 2 || session_->viewChannel() == 2) return true;
    return isotone::band_affects_channel(*b, viewChannel());
}

// A Gaussian across neighbouring points, in dB. Settings, General, Spectrum,
// Smoothing: 0 leaves the points alone, 1 is a window about an octave wide (the
// points are 1/32 of a decade apart), and the ends average over what there is.
void ResponseGraph::smoothForDisplay(std::vector<double>& db, double amount) {
    const double sigma = std::clamp(amount, 0.0, 1.0) * 8.0;
    if (sigma <= 0.05) return;
    const std::ptrdiff_t radius = static_cast<std::ptrdiff_t>(std::ceil(3.0 * sigma));
    const std::vector<double> in = db;
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(in.size());
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        double sum = 0.0, weight = 0.0;
        for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
            const std::ptrdiff_t j = i + k;
            if (j < 0 || j >= n) continue;
            const double w = std::exp(-static_cast<double>(k * k) / (2.0 * sigma * sigma));
            sum += in[static_cast<size_t>(j)] * w;
            weight += w;
        }
        db[static_cast<size_t>(i)] = sum / weight;
    }
}

// A Catmull-Rom curve through the points, as cubic Bezier segments.
QPainterPath ResponseGraph::curveThrough(const std::vector<QPointF>& points) {
    QPainterPath path;
    if (points.empty()) return path;
    path.moveTo(points.front());
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(points.size());
    for (std::ptrdiff_t i = 0; i + 1 < n; ++i) {
        const QPointF& p1 = points[static_cast<size_t>(i)];
        const QPointF& p2 = points[static_cast<size_t>(i + 1)];
        const QPointF& p0 = points[static_cast<size_t>(std::max<std::ptrdiff_t>(i - 1, 0))];
        const QPointF& p3 = points[static_cast<size_t>(std::min<std::ptrdiff_t>(i + 2, n - 1))];
        path.cubicTo(p1 + (p2 - p0) / 6.0, p2 - (p3 - p1) / 6.0, p2);
    }
    return path;
}

void ResponseGraph::paint(QPainter* p) {
    const double h = height();
    const double pw = plotWidth(), ph = plotHeight();
    if (pw <= 0 || ph <= 0) return;
    ++paint_count_;
    p->setRenderHint(QPainter::Antialiasing, true);
    const double max_hz = drawnMaxHz();

    if (part_ == All || part_ == Grid) {
        // Grid: the decades strongest, every other label next, the rest faint, each
        // drawn over the fainter ones.
        const std::vector<double> labels = labelFrequencies(min_hz_, max_hz);
        const auto vertical = [&](double hz, const QColor& colour) {
            p->setPen(QPen(colour, 1.0));
            const double x = std::round(xOf(hz)) + 0.5;
            p->drawLine(QPointF(x, kTop), QPointF(x, kTop + ph));
        };
        const auto labelled = [&](double hz) { return std::find(labels.begin(), labels.end(), hz) != labels.end(); };
        const std::vector<GridLine> lines = gridLines(min_hz_, max_hz);
        for (const GridLine& line : lines) {
            if (!labelled(line.hz) && !isDecade(line.hz)) vertical(line.hz, grid_minor_);
        }
        for (double f : labels) {
            if (!isDecade(f)) vertical(f, separatorColour(grid_major_, zero_line_));
        }
        for (const GridLine& line : lines) {
            if (isDecade(line.hz)) vertical(line.hz, zero_line_);
        }
        for (double f : labels) {
            if (isDecade(f)) vertical(f, zero_line_);
        }

        QFont font(font_family_);
        font.setPixelSize(11);
        font.setFeature(QFont::Tag("tnum"), 1);
        p->setFont(font);
        const QFontMetricsF fm(font);
        p->setPen(label_colour_);
        // At the plot's edges a label is aligned inside it; one that would touch the
        // previous label is left out.
        double previous_right = -1e9;
        for (double f : labels) {
            const QString label = f >= 1000 ? QStringLiteral("%1k").arg(f / 1000) : QString::number(f);
            const double tw = fm.horizontalAdvance(label);
            const double at = xOf(f);
            const double x = at <= kLeft + 1 ? at : at >= kLeft + pw - 1 ? at - tw : at - tw / 2;
            if (x < previous_right + 8) continue;
            p->drawText(QPointF(x, h - 10), label);
            previous_right = x + tw;
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
    }

    const size_t n = static_cast<size_t>(std::max(64.0, pw));
    std::vector<double> freqs(n);
    const double decades = std::log(max_hz / min_hz_);
    for (size_t i = 0; i < n; ++i) freqs[i] = min_hz_ * std::exp(decades * static_cast<double>(i) / static_cast<double>(n - 1));
    const auto x_at = [&](size_t i) { return kLeft + pw * static_cast<double>(i) / static_cast<double>(n - 1); };

    const bool muted = session_ && session_->state().mute;

    // Spectrum on its own scale: the session's top at the top of the plot,
    // kSpectrumRangeDb under it at the bottom. It was 0 to -90, which left music in
    // the bottom third and made an EQ change barely move it; a fixed top then cut
    // off the loud passages (owner, 2026-09-15).
    // None while no audio arrives (the engine is idle). kSpectrumPoints of them
    // whatever the width, smoothed across neighbours and drawn as a curve: a point
    // per pixel drew every harmonic of the FFT as a spike (owner, 2026-09-15).
    const size_t sn = kSpectrumPoints;
    std::vector<double> sfreqs(sn), spectrum(sn);
    for (size_t i = 0; i < sn; ++i)
        sfreqs[i] = min_hz_ * std::exp(decades * static_cast<double>(i) / static_cast<double>(sn - 1));
    const auto spectrum_x = [&](size_t i) { return kLeft + pw * static_cast<double>(i) / static_cast<double>(sn - 1); };
    const double top_db = session_ ? session_->spectrumTopDb() : 0.0;
    const auto spectrum_path = [&](std::vector<double>& db) {
        smoothForDisplay(db, spectrum_smoothing_);
        std::vector<QPointF> points(db.size());
        for (size_t i = 0; i < db.size(); ++i)
            points[i] = QPointF(spectrum_x(i),
                                std::clamp(kTop + ph * (top_db - db[i]) / kSpectrumRangeDb, kTop, kTop + ph));
        return curveThrough(points);
    };
    if ((part_ == All || part_ == Spectrum) && spectrum_visible_ && !muted && session_ &&
        session_->spectrumLevels(sfreqs.data(), sn, spectrum.data())) {
        const QPainterPath edge = spectrum_path(spectrum);
        QPainterPath area = edge;
        area.lineTo(kLeft + pw, kTop + ph);
        area.lineTo(kLeft, kTop + ph);
        area.closeSubpath();
        // Clipped to the plot: the points are inside it, but a curve between two of
        // them that sit on an edge overshoots by a few pixels (owner, 2026-09-15).
        p->save();
        p->setClipRect(QRectF(kLeft, kTop, pw, ph));
        p->fillPath(area, spectrum_fill_);
        p->strokePath(edge, QPen(spectrum_edge_, 1.0));
        p->restore();
    }
    if (!session_ || part_ == Grid || part_ == Spectrum) return;

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
    // Surround: a band on another channel in view sits on that channel's line,
    // drawn as the right is in L+R where it differs, marked with the speaker codes.
    if (channels > 2 && !state.bypass) {
        const uint32_t primary = viewChannel();
        std::vector<uint32_t> others;
        for (int row = 0; row < session_->rowCount(); ++row) {
            const uint32_t c = isotone::ui::band_view_channel(*session_->bandAt(row), channels, session_->showingMask(), primary);
            if (c != primary && std::find(others.begin(), others.end(), c) == others.end()) others.push_back(c);
        }
        const std::vector<isotone::ui::Speaker> speakers = isotone::ui::layout_speakers(channels, mask);
        QFont bold(font_family_);
        bold.setPixelSize(11);
        bold.setWeight(QFont::DemiBold);
        p->setFont(bold);
        const double lx = kLeft + pw - 14 - QFontMetricsF(bold).horizontalAdvance(QStringLiteral("LFE"));
        bool marked = false;
        for (uint32_t c : others) {
            std::vector<double> line(n);
            isotone::magnitude_db(drawn, channels, mask, c, freqs.data(), n, kSampleRate, line.data());
            bool differs = false;
            for (size_t i = 0; i < n && !differs; ++i) differs = std::abs(line[i] - db[i]) > 0.01;
            if (!differs) continue;
            QPen dashed(accent_, 2.5);
            dashed.setJoinStyle(Qt::RoundJoin);
            dashed.setCapStyle(Qt::FlatCap);
            dashed.setDashPattern({7.0 / 2.5, 5.0 / 2.5});
            p->strokePath(polyline(line), dashed);
            p->setPen(accent_);
            p->drawText(QPointF(lx, std::clamp(yOf(line[n - 1]) + 16, kTop + 10, kTop + ph)),
                        QString::fromStdString(speakers[c].code));
            marked = true;
        }
        if (marked) {
            p->setPen(accent_);
            p->drawText(QPointF(lx, std::clamp(yOf(db[n - 1]) - 8, kTop + 10, kTop + ph)),
                        QString::fromStdString(speakers[primary].code));
        }
    }
    QPen pen(accent_, 2.5);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p->strokePath(curve, pen);
}
