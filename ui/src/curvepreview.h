// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The curve of an import preview: grid, labels, each band's bell and the
// composite with its fill, drawn from the core as ResponseGraph draws them,
// without handles, spectrum or preamp.

#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include "importpreview.h"

class CurvePreview : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ImportPreview* preview READ preview WRITE setPreview NOTIFY previewChanged)
    Q_PROPERTY(QColor accent MEMBER accent_ NOTIFY styleChanged)
    Q_PROPERTY(QColor gridMajor MEMBER grid_major_ NOTIFY styleChanged)
    Q_PROPERTY(QColor gridMinor MEMBER grid_minor_ NOTIFY styleChanged)
    Q_PROPERTY(QColor zeroLine MEMBER zero_line_ NOTIFY styleChanged)
    Q_PROPERTY(QColor labelColour MEMBER label_colour_ NOTIFY styleChanged)
    Q_PROPERTY(QColor bell MEMBER bell_ NOTIFY styleChanged)
    Q_PROPERTY(double fillEdgeAlpha MEMBER fill_edge_alpha_ NOTIFY styleChanged)
    Q_PROPERTY(double fillMidAlpha MEMBER fill_mid_alpha_ NOTIFY styleChanged)
    Q_PROPERTY(QString fontFamily MEMBER font_family_ NOTIFY styleChanged)

public:
    explicit CurvePreview(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    ImportPreview* preview() const { return preview_; }
    void setPreview(ImportPreview* p);

    // The composite at `hz` on the first channel, without preamp.
    Q_INVOKABLE double compositeAt(double hz) const;

signals:
    void previewChanged();
    void styleChanged();

private:
    // The rate the curve is designed at: that of the output the file is for.
    double sampleRate() const;

    QPointer<ImportPreview> preview_;
    QColor accent_{0x6a, 0xa7, 0xf4};
    QColor grid_major_, grid_minor_, zero_line_, label_colour_, bell_;
    double fill_edge_alpha_ = 0.30, fill_mid_alpha_ = 0.04;
    QString font_family_;
};
