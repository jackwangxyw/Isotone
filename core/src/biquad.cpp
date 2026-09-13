// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/biquad.h"

#include <algorithm>
#include <cmath>

namespace isotone {
namespace {

constexpr double kPi  = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;

// The narrowest band a bandwidth may describe, as an equivalent Q. sinh grows
// so fast that a wide bandwidth near Nyquist rounds a2 to exactly -1, poles on
// the unit circle; this is the same floor the processor puts under Q.
constexpr double kMinEquivalentQ = 1e-4;

bool finite(double v) { return std::isfinite(v); }

// Width term of the RBJ formulas. alpha sets how wide in frequency the filter
// acts; every type below uses it. The three branches are the three ways a config
// file is allowed to express that width (verified against upstream BiQuad.cpp).
double alpha_for(WidthMode mode, double width, double sin_w0, double w0, double A, FilterType type) {
    switch (mode) {
        case WidthMode::Q:
            return sin_w0 / (2.0 * width);
        case WidthMode::BandwidthOct:
            // sinh form; the w0/sin(w0) factor undoes the bilinear frequency warp
            // so the -3 dB points land a true `width` octaves apart.
            return std::min(sin_w0 * std::sinh(kLn2 / 2.0 * width * w0 / sin_w0), sin_w0 / (2.0 * kMinEquivalentQ));
        case WidthMode::SlopeDb: {
            // S is normalised so S = 1 is the steepest shelf without overshoot.
            // Upstream divides a dB-per-octave slope by 12 to get S.
            if (type != FilterType::LowShelf && type != FilterType::HighShelf) {
                return sin_w0 / (2.0 * width);
            }
            const double S = width / 12.0;
            // Past S = 1 the term under the root falls as the gain rises, reaching
            // zero (poles on the unit circle) and then going negative (NaN). Held
            // at the value for an equivalent Q of 10, so a steep shelf stays a
            // stable, if resonant, filter at any gain.
            const double inner = (A + 1.0 / A) * (1.0 / S - 1.0) + 2.0;
            return sin_w0 / 2.0 * std::sqrt(std::max(inner, 0.01));
        }
    }
    return sin_w0 / (2.0 * width);
}

// Equalizer APO's LS/HS tokens treat fc as the shelf corner rather than its
// midpoint, and compensate by shifting the design frequency. Upstream calls this
// the DCX2496 adjustment (BiQuadFilter::initialize). LSC/HSC skip it. Reproduced
// exactly so imported configs measure identically here.
double shelf_corner_freq(const Band& band, double fc, double A) {
    double S = band.width / 12.0;
    if (band.width_mode == WidthMode::Q) {
        const double q = band.width;
        S = 1.0 / ((1.0 / (q * q) - 2.0) / (A + 1.0 / A) + 1.0);
    } else if (band.width_mode == WidthMode::BandwidthOct) {
        return fc;  // upstream ignores bandwidth for shelves
    }
    if (!finite(S) || S == 0.0) {
        return fc;
    }
    const double factor = std::pow(10.0, std::abs(band.gain_db) / 80.0 / S);
    if (!finite(factor) || factor <= 0.0) {
        return fc;
    }
    return band.type == FilterType::LowShelf ? fc * factor : fc / factor;
}

}  // namespace

double clamp_fc(double fc, double sample_rate) {
    const double max_fc = sample_rate * 0.5 * kMaxFcOfNyquist;
    return std::clamp(fc, kMinFc, std::max(kMinFc, max_fc));
}

BiquadCoeffs butterworth2(FilterType type, double fc, double sample_rate) {
    Band b;
    b.type  = type;
    b.fc    = fc;
    b.width = std::sqrt(0.5);
    return design(b, sample_rate);
}

BiquadCoeffs design(const Band& band, double sample_rate) {
    if (!band.enabled || !(sample_rate > 0.0) || !finite(sample_rate)) {
        return BiquadCoeffs::identity();
    }
    if (!finite(band.fc) || !finite(band.gain_db) || !finite(band.width) || band.width <= 0.0) {
        return BiquadCoeffs::identity();
    }

    const bool is_gain_type = band.type == FilterType::Peaking ||
                              band.type == FilterType::LowShelf ||
                              band.type == FilterType::HighShelf;

    // A is the amplitude the filter aims for. Note the /40 rather than /20:
    // peaking and shelving sections apply A to the numerator and 1/A to the
    // denominator, so the two together produce the requested gain_db.
    const double A = is_gain_type ? std::pow(10.0, band.gain_db / 40.0)
                                  : std::pow(10.0, band.gain_db / 20.0);

    double fc = clamp_fc(band.fc, sample_rate);
    const bool is_shelf = band.type == FilterType::LowShelf || band.type == FilterType::HighShelf;
    if (is_shelf && band.shelf_corner) {
        fc = clamp_fc(shelf_corner_freq(band, fc, A), sample_rate);
    }

    const double w0     = 2.0 * kPi * fc / sample_rate;
    const double sin_w0 = std::sin(w0);
    const double cos_w0 = std::cos(w0);

    if (sin_w0 == 0.0) {
        return BiquadCoeffs::identity();
    }

    const double alpha = alpha_for(band.width_mode, band.width, sin_w0, w0, A, band.type);
    if (!finite(alpha)) {
        return BiquadCoeffs::identity();
    }
    const double beta = 2.0 * std::sqrt(A) * alpha;

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (band.type) {
        case FilterType::LowPass:
            b0 = (1.0 - cos_w0) / 2.0;
            b1 = 1.0 - cos_w0;
            b2 = (1.0 - cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case FilterType::HighPass:
            b0 = (1.0 + cos_w0) / 2.0;
            b1 = -(1.0 + cos_w0);
            b2 = (1.0 + cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case FilterType::BandPass:
            // Constant 0 dB peak gain: the passband tops out at unity whatever Q
            // is, and Q only sets the width. RBJ also defines a constant-skirt
            // variant (b0 = sin(w0)/2) whose peak is Q; upstream Equalizer APO
            // uses this one, so we do too.
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case FilterType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cos_w0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case FilterType::AllPass:
            b0 = 1.0 - alpha;
            b1 = -2.0 * cos_w0;
            b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case FilterType::Peaking:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cos_w0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha / A;
            break;
        case FilterType::LowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + beta);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - beta);
            a0 = (A + 1.0) + (A - 1.0) * cos_w0 + beta;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
            a2 = (A + 1.0) + (A - 1.0) * cos_w0 - beta;
            break;
        case FilterType::HighShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + beta);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - beta);
            a0 = (A + 1.0) - (A - 1.0) * cos_w0 + beta;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
            a2 = (A + 1.0) - (A - 1.0) * cos_w0 - beta;
            break;
    }

    if (a0 == 0.0 || !finite(a0)) {
        return BiquadCoeffs::identity();
    }

    BiquadCoeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;

    if (!finite(c.b0) || !finite(c.b1) || !finite(c.b2) || !finite(c.a1) || !finite(c.a2)) {
        return BiquadCoeffs::identity();
    }
    return c;
}

std::complex<double> response(const BiquadCoeffs& c, double freq, double sample_rate) {
    if (!(freq > 0.0) || !(sample_rate > 0.0) || freq > sample_rate * 0.5) {
        return {1.0, 0.0};
    }
    const double w = 2.0 * kPi * freq / sample_rate;
    // z^-1 = e^{-jw}, z^-2 = e^{-2jw}: one and two samples of delay.
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = std::polar(1.0, -2.0 * w);
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
    if (den == std::complex<double>(0.0, 0.0)) {
        return {1.0, 0.0};
    }
    return num / den;
}

}  // namespace isotone
