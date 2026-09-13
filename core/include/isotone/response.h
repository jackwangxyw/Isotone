// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Composite frequency response of an EqState. This is the code that draws the
// curve in the UI; the hosts run the same design() it is built on, so the curve
// drawn is the curve heard (plan 4.5).

#pragma once

#include <cstddef>
#include <vector>

#include "isotone/types.h"

namespace isotone {

// Magnitude in dB of every band active on `channel`, plus preamp and that
// channel's trim. Under bypass, the bands and preamp are left out and the trim
// and mute kept, as the processor does. Writes `n` values into `out`.
void magnitude_db(const EqState& state, uint32_t channel, const double* freqs, size_t n,
                  double sample_rate, double* out);

// Unwrapped-free phase in degrees, same selection of bands. Preamp and trims are
// real gains and contribute no phase.
void phase_deg(const EqState& state, uint32_t channel, const double* freqs, size_t n,
               double sample_rate, double* out);

// Magnitude of one band alone, for drawing individual bells under the composite.
// Ignores preamp, trims, mute and bypass; honours band.enabled.
void band_magnitude_db(const Band& band, const double* freqs, size_t n, double sample_rate,
                       double* out);

// Peak output level over all channels, relative to a full-scale input, evaluated
// on `freqs` plus every band centre frequency (narrow high-Q peaks fall between
// grid points). Includes the speaker setup: routing and bass management sum
// channels, so an output can reach the sum of every path into it (the sub takes
// the bass of each small speaker). Per output and frequency this is the most any
// set of full-scale inputs can produce, reached when they are in phase. Preamp
// is left out. This is the quantity auto-preamp negates (plan 4.6).
double composite_peak_db(const EqState& state, uint32_t channels, uint32_t speaker_mask,
                         const double* freqs, size_t n, double sample_rate);

// `count` log-spaced points from `f_lo` to `f_hi` inclusive.
std::vector<double> log_grid(double f_lo, double f_hi, size_t count);

}  // namespace isotone
