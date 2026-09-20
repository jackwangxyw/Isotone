// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Isotone mark as an icon, for the tray and the window: five bars about the
// zero line on a light plate, drawn at each tray, taskbar and Alt+Tab size
// (docs/design/logo, locked 2026-09-19).
//
// The same geometry ships three more ways, and all four come from one set of
// numbers: tools/gen_icons.py cuts res/isotone.ico, the installer's bitmaps and
// the Linux icon theme; qml/Icon.qml's "logo" strokes it as five round-capped
// segments. ui/tests/test_logomark.cpp fails if any of them drifts.

#pragma once

#include <QIcon>
#include <QRectF>

class QColor;
class QPainter;

QIcon logo_mark_icon();

// The bars alone, filling `box`, with `inset` of it left as a margin. Exposed
// so a test can measure what is drawn rather than only look at it.
void paint_logo_mark(QPainter* p, const QRectF& box, const QColor& glyph, double inset = 0.0);
