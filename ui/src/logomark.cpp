// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "logomark.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

QIcon logo_mark_icon() {
    QIcon icon;
    for (int size : {16, 20, 24, 32, 40, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf3, 0xf5, 0xf8));
        p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.25, size * 0.25);
        // "M3 14c3 0 3-6 6-6s3 8 6 8 3-4 6-4" in absolute points, drawn in the
        // middle 16/28 of the square as the sidebar's mark is.
        QPainterPath wave(QPointF(3, 14));
        wave.cubicTo(6, 14, 6, 8, 9, 8);
        wave.cubicTo(12, 8, 12, 16, 15, 16);
        wave.cubicTo(18, 16, 18, 12, 21, 12);
        const double scale = size * (16.0 / 28.0) / 24.0;
        p.translate(size / 2.0, size / 2.0);
        p.scale(scale, scale);
        p.translate(-12, -12);
        QPen pen(QColor(0x12, 0x15, 0x19), size >= 32 ? 2.2 : 2.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(wave);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
