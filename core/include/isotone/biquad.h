// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Second-order IIR ("biquad") section: design from a Band, and evaluation of
// its frequency response. Designed in double throughout.
//
// The design formulas are the RBJ Audio EQ Cookbook, matching upstream
// Equalizer APO's filters/BiQuad.cpp so that an imported config sounds the same
// here as it does there.

#pragma once

#include <complex>
#include <cstddef>

#include "isotone/types.h"

namespace isotone {

// Normalised direct-form coefficients: a0 has already been divided out, so the
// difference equation is
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;

    static BiquadCoeffs identity() { return BiquadCoeffs{}; }
};

// Lower bound on centre frequency, and the fraction of nyquist above which fc is
// clamped. Both applied at design time, per plan 4.1.
inline constexpr double kMinFc          = 10.0;
inline constexpr double kMaxFcOfNyquist = 0.95;

// Clamp fc into the range this sample rate can represent.
double clamp_fc(double fc, double sample_rate);

// Design a single biquad. Returns identity coefficients for a disabled band, a
// non-finite parameter, or a non-positive sample rate.
BiquadCoeffs design(const Band& band, double sample_rate);

// H(e^{jw}) at `freq` Hz. Returns 1+0i for freq outside (0, nyquist].
std::complex<double> response(const BiquadCoeffs& c, double freq, double sample_rate);

inline double magnitude_db(const BiquadCoeffs& c, double freq, double sample_rate) {
    return 20.0 * std::log10(std::abs(response(c, freq, sample_rate)));
}

inline double phase_deg(const BiquadCoeffs& c, double freq, double sample_rate) {
    return std::arg(response(c, freq, sample_rate)) * (180.0 / 3.14159265358979323846);
}

// True if both poles are strictly inside the unit circle, i.e. the section is
// stable. Uses the Jury/Schur triangle conditions on (a1, a2), which for a
// second-order section reduce to these three inequalities.
inline bool is_stable(const BiquadCoeffs& c) {
    return std::abs(c.a2) < 1.0 && std::abs(c.a1) < 1.0 + c.a2;
}

}  // namespace isotone
