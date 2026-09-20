// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "logomark.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {

// The mark, in the 24 x 24 box it is specified in (docs/design/logo, locked
// 2026-09-19). Five bars hung from the zero line at y = 12, then shifted so the
// ink is centred: the bars are not symmetric about that line, so centring on it
// sits the mark high in a tile.
//
// These are the same numbers tools/gen_icons.py cuts every shipped icon from,
// and test_logomark.cpp fails if the two disagree.
constexpr double kBox = 24.0;
constexpr double kBarWidth = 3.0;
constexpr double kTileRadius = 0.22;   // of the tile's side
constexpr double kGlyphInset = 0.14;   // the glyph's margin inside the tile

struct Bar {
    double x, top, width, height;
};

constexpr Bar kBars[] = {
    {3.10, 11.61, kBarWidth, 4.83},
    {6.80, 5.71, kBarWidth, 8.90},
    {10.50, 11.61, kBarWidth, 6.68},
    {14.20, 7.93, kBarWidth, 6.68},
    {17.90, 11.61, kBarWidth, 4.83},
};

}  // namespace

void paint_logo_mark(QPainter* p, const QRectF& box, const QColor& glyph, double inset) {
    const double scale = box.width() / kBox * (1.0 - inset);
    const double offset = box.width() * inset / 2.0;
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    p->setBrush(glyph);
    for (const Bar& bar : kBars) {
        const QRectF r(box.x() + offset + bar.x * scale, box.y() + offset + bar.top * scale,
                       bar.width * scale, bar.height * scale);
        const double radius = kBarWidth / 2.0 * scale;
        p->drawRoundedRect(r, radius, radius);
    }
    p->restore();
}

QIcon logo_mark_icon() {
    QIcon icon;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        // The light plate, so the mark reads on a dark taskbar and on a light one.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf3, 0xf5, 0xf8));
        p.drawRoundedRect(QRectF(0, 0, size, size), size * kTileRadius, size * kTileRadius);
        paint_logo_mark(&p, QRectF(0, 0, size, size), QColor(0x1b, 0x20, 0x25), kGlyphInset);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
