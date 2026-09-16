// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Importing a magnitude curve: the two formats that carry one instead of
// filters, and peaking bands whose composite follows it.
//
//   GraphicEQ: 20 -1.2; 21 -1.3; ...        Equalizer APO, what AutoEQ writes
//   FilterCurve:f0="10" ... v0="-39.9" ...  Audacity, and Peace's export
//
// Equalizer APO plays these as a convolution filter. Isotone's engine is
// biquads, so a curve is fitted: bands on a fixed third-octave grid whose gains
// are solved for by least squares (owner's TC8FD05-04 export, 2026-09-15).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "isotone/types.h"

namespace isotone {

struct CurvePoint {
    double hz = 0;
    double db = 0;
};

// The curve a GraphicEQ or FilterCurve line holds, in frequency order, or empty
// when `text` has neither. Points out of order are sorted; a point with a
// frequency at or below 0, or a value that is not a number, is left out.
std::vector<CurvePoint> parse_curve(const std::string& text);

struct CurveFit {
    std::vector<Band> bands;
    // How far the fitted composite is from the curve over the curve's own range,
    // in dB: the worst point, and the root mean square.
    double worst_db = 0;
    double rms_db = 0;
};

// Filters whose composite follows `curve`, in frequency order: a high-pass and a
// high shelf where the curve has those shapes, then as few peaking bands as
// follow the rest. Each band is the one that takes most of the remaining error
// out, chosen from a sixth-octave grid of frequencies and seven widths, until the
// curve is followed within about a third of a dB or twelve bands are used; their
// gains are then solved together and refined against the real composite (a
// peaking filter's shape widens a little with gain, so one linear solve is not
// enough) and clamped to `max_gain_db`. An export of a handful of filters comes
// back as a handful. A flat curve gives no bands; fewer than two points gives an
// empty fit.
CurveFit fit_curve(const std::vector<CurvePoint>& curve, double sample_rate = 48000.0, double max_gain_db = 24.0);

}  // namespace isotone
