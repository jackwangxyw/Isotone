// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The frequency response graph: grid, spectrum, per-band bells, composite curve
// and fill, drawn from the core's magnitude_db and band_magnitude_db. Handles,
// hover readout and interaction are QML on top, placed with xOf/yOf.
//
// The curve leaves the preamp out (owner's decision, 2026-09-14); with the EQ
// off it is flat, and compositeAt still gives where the bands would put it, for
// the faded handles.

#pragma once

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <vector>

class EqSession;

class ResponseGraph : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(EqSession* session READ session WRITE setSession NOTIFY sessionChanged)
    // The part this item draws: all of it, or the Grid, Spectrum or Curves, as
    // layers stacked in that order, so that a new spectrum frame repaints the
    // spectrum alone. Repainting the grid and the curves 60 times a second took
    // 15.6 ms a frame in a maximized window, the spectrum 1.1 of it (2026-09-16).
    Q_PROPERTY(Part part READ part WRITE setPart NOTIFY styleChanged)
    // Settings, General, Graph: the gain range (12, 15 or 24 dB) and the frequency
    // range. Each change bumps `revision`, so handles and the readout follow.
    Q_PROPERTY(double rangeDb READ rangeDb WRITE setRangeDb NOTIFY rangeChanged)
    Q_PROPERTY(double minHz READ minHz WRITE setMinHz NOTIFY rangeChanged)
    Q_PROPERTY(double maxHz READ maxHz WRITE setMaxHz NOTIFY rangeChanged)
    // Settings, General, Spectrum: how much the drawn curve is smoothed, 0 (the
    // points as they are) to 1 (the widest window).
    Q_PROPERTY(double spectrumSmoothing MEMBER spectrum_smoothing_ NOTIFY styleChanged)
    Q_PROPERTY(bool spectrumVisible MEMBER spectrum_visible_ NOTIFY styleChanged)
    Q_PROPERTY(bool perBandColours MEMBER per_band_ NOTIFY styleChanged)
    Q_PROPERTY(QVariantList bandColours MEMBER band_colours_ NOTIFY styleChanged)
    Q_PROPERTY(QColor accent MEMBER accent_ NOTIFY styleChanged)
    Q_PROPERTY(QColor gridMajor MEMBER grid_major_ NOTIFY styleChanged)
    Q_PROPERTY(QColor gridMinor MEMBER grid_minor_ NOTIFY styleChanged)
    Q_PROPERTY(QColor zeroLine MEMBER zero_line_ NOTIFY styleChanged)
    Q_PROPERTY(QColor labelColour MEMBER label_colour_ NOTIFY styleChanged)
    Q_PROPERTY(QColor spectrumFill MEMBER spectrum_fill_ NOTIFY styleChanged)
    Q_PROPERTY(QColor spectrumEdge MEMBER spectrum_edge_ NOTIFY styleChanged)
    Q_PROPERTY(QColor bell MEMBER bell_ NOTIFY styleChanged)
    Q_PROPERTY(double fillEdgeAlpha MEMBER fill_edge_alpha_ NOTIFY styleChanged)
    Q_PROPERTY(double fillMidAlpha MEMBER fill_mid_alpha_ NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER font_family_ NOTIFY styleChanged)

    Q_PROPERTY(double plotLeft READ plotLeft CONSTANT)
    Q_PROPERTY(double plotTop READ plotTop CONSTANT)
    Q_PROPERTY(double plotWidth READ plotWidth NOTIFY geometryChanged)
    Q_PROPERTY(double plotHeight READ plotHeight NOTIFY geometryChanged)
    // Bumped whenever the curve changes, so QML bindings on xOf/yOf/compositeAt re-run.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    enum Part { All, Grid, Spectrum, Curves };
    Q_ENUM(Part)

    explicit ResponseGraph(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    Part part() const { return part_; }
    void setPart(Part part);
    // How many times it has painted, for tests.
    Q_INVOKABLE int paintCount() const { return paint_count_; }

    EqSession* session() const { return session_; }
    void setSession(EqSession* s);

    double rangeDb() const { return range_db_; }
    void setRangeDb(double db);
    double minHz() const { return min_hz_; }
    void setMinHz(double hz);
    double maxHz() const { return max_hz_; }
    void setMaxHz(double hz);

    // The vertical grid for a frequency range: 1, 1.5, 2, 3, 4, 5, 6 and 8 in each
    // decade, near an eighth of a decade apart (squig.link's lines), major at 1, 2
    // and 5. Drawn in three weights (owner, 2026-09-16): the decades (100 Hz, 1 kHz,
    // 10 kHz) strongest, every other label next, the rest faint.
    struct GridLine {
        double hz;
        bool major;
    };
    static std::vector<GridLine> gridLines(double min_hz, double max_hz);
    // Where the frequency labels go: the major lines, or every line when the
    // range holds fewer than three, or the range's ends when it holds fewer than two.
    static std::vector<double> labelFrequencies(double min_hz, double max_hz);
    // A label's separator: a quarter of the way from the major grid colour to the zero
    // line's, a tad over the minor lines (owner, 2026-09-16). A decade's is the zero
    // line's colour.
    static QColor separatorColour(const QColor& grid_major, const QColor& zero_line);
    // A power of ten.
    static bool isDecade(double hz);

    double plotLeft() const { return kLeft; }
    double plotTop() const { return kTop; }
    double plotWidth() const { return width() - kLeft - kRight; }
    double plotHeight() const { return height() - kTop - kBottom; }
    int revision() const { return revision_; }

    Q_INVOKABLE double xOf(double hz) const;
    Q_INVOKABLE double yOf(double db) const;
    Q_INVOKABLE double frequencyAt(double x) const;
    Q_INVOKABLE double dbAt(double y) const;
    // The composite of the enabled bands at `hz`, as the handles sit on it:
    // without preamp and bypass.
    Q_INVOKABLE double compositeAt(double hz) const;
    // Where band `row`'s handle sits: the composite at its frequency on the channel
    // it is drawn on (in L+R, a band on the right alone is on the right).
    Q_INVOKABLE double handleDb(int row) const;
    // False for a band not on the channel in view.
    Q_INVOKABLE bool onView(int row) const;

    // How the spectrum is drawn (tested in ui_tests): this many points whatever
    // the plot's width, on a scale that runs from the session's spectrumTopDb at
    // the top of the plot down over kSpectrumRangeDb, a Gaussian across
    // neighbouring points in dB whose width `amount` (0 to 1) sets, and a
    // Catmull-Rom curve through them.
    static constexpr size_t kSpectrumPoints = 320;
    static constexpr double kSpectrumRangeDb = 60.0;   // EqSession::kSpectrumRangeDb, the same scale
    static void smoothForDisplay(std::vector<double>& db, double amount);

    // Which of `y` a polyline needs to stay within `tol` of all of it:
    // Ramer-Douglas-Peucker on the vertical distance alone, which is the whole
    // error where x is one sample per pixel and monotonic. The ends are always
    // kept. Drawing a band's bell is dominated by its segment count, and most of
    // a bell is flat, so this is what keeps a drag cheap with many bands.
    static std::vector<uint8_t> simplifyKeep(const std::vector<double>& y, double tol);
    static QPainterPath curveThrough(const std::vector<QPointF>& points);

signals:
    void sessionChanged();
    void styleChanged();
    void geometryChanged();
    void revisionChanged();
    void rangeChanged();

protected:
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    // The generator's margins (docs/design/generator/gen_mockups3.py, graph_svg).
    static constexpr double kLeft = 46.0, kRight = 14.0, kTop = 14.0, kBottom = 30.0;

    void curveChanged();
    // The channel the composite is drawn for: the right in R view, else the left.
    uint32_t viewChannel() const;
    double compositeOn(uint32_t channel, double hz) const;
    // The range as drawn: an inverted one (between two settings) spans an octave.
    double drawnMaxHz() const { return max_hz_ > min_hz_ ? max_hz_ : min_hz_ * 2.0; }

    QPointer<EqSession> session_;
    Part part_ = All;
    std::atomic<int> paint_count_{0};
    int revision_ = 0;
    double range_db_ = 15.0;
    double min_hz_ = 20.0, max_hz_ = 20000.0;
    bool spectrum_visible_ = true;
    double spectrum_smoothing_ = 0.35;
    bool per_band_ = true;
    QVariantList band_colours_;
    QColor accent_{0x6a, 0xa7, 0xf4};
    QColor grid_major_, grid_minor_, zero_line_, label_colour_, spectrum_fill_, spectrum_edge_, bell_;
    double fill_edge_alpha_ = 0.30, fill_mid_alpha_ = 0.04;
    QString font_family_;
};
