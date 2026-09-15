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
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class EqSession;

class ResponseGraph : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(EqSession* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(double rangeDb MEMBER range_db_ NOTIFY styleChanged)
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
    explicit ResponseGraph(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    EqSession* session() const { return session_; }
    void setSession(EqSession* s);

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

signals:
    void sessionChanged();
    void styleChanged();
    void geometryChanged();
    void revisionChanged();

protected:
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    // The generator's margins (docs/design/generator/gen_mockups3.py, graph_svg).
    static constexpr double kLeft = 46.0, kRight = 14.0, kTop = 14.0, kBottom = 30.0;

    void curveChanged();
    // The channel the composite is drawn for: the right in R view, else the left.
    uint32_t viewChannel() const;
    double compositeOn(uint32_t channel, double hz) const;

    QPointer<EqSession> session_;
    int revision_ = 0;
    double range_db_ = 15.0;
    bool spectrum_visible_ = true;
    bool per_band_ = true;
    QVariantList band_colours_;
    QColor accent_{0x6a, 0xa7, 0xf4};
    QColor grid_major_, grid_minor_, zero_line_, label_colour_, spectrum_fill_, spectrum_edge_, bell_;
    double fill_edge_alpha_ = 0.30, fill_mid_alpha_ = 0.04;
    QString font_family_;
};
