// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// A filter type's tile glyph in the band popover: the response of one band of
// that type, from the core, with the generator's parameters (gen_screens.py,
// TYPE_TILES and type_glyph). The all pass draws its flat magnitude and, fainter,
// its phase turning through 360 degrees.

#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class FilterGlyph : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int type MEMBER type_ NOTIFY changed)   // isotone::FilterType
    Q_PROPERTY(QColor stroke MEMBER stroke_ NOTIFY changed)
    Q_PROPERTY(QColor zeroLine MEMBER zero_line_ NOTIFY changed)
    Q_PROPERTY(QColor background MEMBER background_ NOTIFY changed)
    Q_PROPERTY(bool selected MEMBER selected_ NOTIFY changed)

public:
    explicit FilterGlyph(QQuickItem* parent = nullptr);
    void paint(QPainter* painter) override;

signals:
    void changed();

private:
    int type_ = 0;
    QColor stroke_, zero_line_, background_;
    bool selected_ = false;
};
