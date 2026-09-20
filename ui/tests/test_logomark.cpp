// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The mark's geometry, which is written down in four places: logomark.cpp draws
// it, qml/Icon.qml strokes it, tools/gen_icons.py cuts the shipped icons from
// it, and docs/design/logo specifies it. Nothing shares the numbers at build
// time, so this is what stops them drifting apart.
//
// Measured out of a rendered pixmap rather than read back from the constants,
// so it fails if the drawing changes as well as if the table does.

#include "doctest.h"

#include <QColor>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <vector>

#include "logomark.h"

namespace {

struct Column {
    int x0, x1, top, bottom;
};

// Every run of columns that has ink in it, with the ink's top and bottom.
std::vector<Column> columns_of(const QImage& image) {
    std::vector<Column> out;
    bool in_bar = false;
    for (int x = 0; x < image.width(); ++x) {
        int top = -1, bottom = -1;
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 128) {
                if (top < 0) top = y;
                bottom = y;
            }
        }
        if (top >= 0 && !in_bar) {
            out.push_back({x, x, top, bottom});
            in_bar = true;
        } else if (top >= 0) {
            out.back().x1 = x;
            out.back().top = std::min(out.back().top, top);
            out.back().bottom = std::max(out.back().bottom, bottom);
        } else {
            in_bar = false;
        }
    }
    return out;
}

QImage rendered(int size, double inset = 0.0) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    paint_logo_mark(&p, QRectF(0, 0, size, size), QColor(255, 255, 255), inset);
    p.end();
    return image;
}

}  // namespace

TEST_CASE("the mark is five bars at the sizes the spec gives") {
    // 240 px: 10 px to the box's unit, so the spec's two decimals land within a
    // pixel and the rounded caps do not have to be reasoned about.
    const int size = 240;
    const double unit = size / 24.0;
    const QImage image = rendered(size);
    const std::vector<Column> bars = columns_of(image);
    REQUIRE(bars.size() == 5);

    // x, top, height, in box units (docs/design/logo/out/mark-locked.png).
    const double expect[5][3] = {
        {3.10, 11.61, 4.83},
        {6.80, 5.71, 8.90},
        {10.50, 11.61, 6.68},
        {14.20, 7.93, 6.68},
        {17.90, 11.61, 4.83},
    };
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        CHECK(bars[i].x0 / unit == doctest::Approx(expect[i][0]).epsilon(0.02));
        CHECK((bars[i].x1 + 1 - bars[i].x0) / unit == doctest::Approx(3.0).epsilon(0.02));
        CHECK(bars[i].top / unit == doctest::Approx(expect[i][1]).epsilon(0.02));
        CHECK((bars[i].bottom + 1 - bars[i].top) / unit == doctest::Approx(expect[i][2]).epsilon(0.02));
    }
}

TEST_CASE("the mark is centred on its ink, not on the zero line") {
    const int size = 240;
    const QImage image = rendered(size);
    const std::vector<Column> bars = columns_of(image);
    REQUIRE(bars.size() == 5);

    int top = image.height(), bottom = -1, left = image.width(), right = -1;
    for (const Column& bar : bars) {
        top = std::min(top, bar.top);
        bottom = std::max(bottom, bar.bottom);
        left = std::min(left, bar.x0);
        right = std::max(right, bar.x1);
    }
    // Equal margins above and below. The bars hang from the zero line at y = 12
    // and are not symmetric about it, so this is only true because the drawing
    // shifts them; centring on the line leaves 19 px more below at this size.
    CHECK(std::abs(top - (size - 1 - bottom)) <= 1);
    CHECK(std::abs(left - (size - 1 - right)) <= 1);
    // And the ink is the size the spec says: 17.80 x 12.58 units.
    const double unit = size / 24.0;
    CHECK((right + 1 - left) / unit == doctest::Approx(17.80).epsilon(0.02));
    CHECK((bottom + 1 - top) / unit == doctest::Approx(12.58).epsilon(0.02));
}

TEST_CASE("the icon carries every size the tray and the taskbar ask for") {
    const QIcon icon = logo_mark_icon();
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        CAPTURE(size);
        const QPixmap pixmap = icon.pixmap(size, size);
        CHECK(pixmap.width() == size);
        // The plate is opaque, so a corner is the tile and the middle is ink.
        const QImage image = pixmap.toImage();
        CHECK(qAlpha(image.pixel(size / 2, size / 2)) > 0);
    }
}

TEST_CASE("the inset leaves the glyph inside the tile") {
    const int size = 240;
    const std::vector<Column> plain = columns_of(rendered(size, 0.0));
    const std::vector<Column> inset = columns_of(rendered(size, 0.14));
    REQUIRE(plain.size() == 5);
    REQUIRE(inset.size() == 5);
    // 14% off the box, so the ink is 86% as wide and still centred.
    const double plain_width = plain.back().x1 - plain.front().x0;
    const double inset_width = inset.back().x1 - inset.front().x0;
    CHECK(inset_width / plain_width == doctest::Approx(0.86).epsilon(0.01));
    CHECK(inset.front().x0 > plain.front().x0);
    CHECK(inset.back().x1 < plain.back().x1);
}
