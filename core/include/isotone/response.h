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

// Magnitude in dB of what output `channel` plays of its own input: every band
// active on it, preamp, its trim, mute and speaker mute (both -inf), and with
// bass management on a stream with an LFE channel, a small speaker's high-pass
// or the LFE channel's low-pass. Routing and the bass other speakers send the
// LFE mix channels together and are left out. `channels` and `speaker_mask` are
// the stream's layout; a mask of 0 means the usual one for the channel count.
// A state written for another layout (EqState::layout_channels) is drawn after
// remap_channels, where the engine plays it; so is it in phase_deg and
// composite_peak_db. Under bypass, the bands and preamp are left out and the rest kept, as the
// processor does. Writes `n` values into `out`.
void magnitude_db(const EqState& state, uint32_t channels, uint32_t speaker_mask, uint32_t channel,
                  const double* freqs, size_t n, double sample_rate, double* out);

// Phase in degrees, in (-180, 180], of the same path: the bands, the bass
// management filters and polarity. Delay is left out; its phase is linear in
// frequency. Preamp, trims and mute are real gains and contribute no phase.
void phase_deg(const EqState& state, uint32_t channels, uint32_t speaker_mask, uint32_t channel,
               const double* freqs, size_t n, double sample_rate, double* out);

// Magnitude of one band alone, for drawing individual bells under the composite.
// Ignores preamp, trims, mute and bypass; honours band.enabled.
void band_magnitude_db(const Band& band, const double* freqs, size_t n, double sample_rate,
                       double* out);

// Peak output level over all channels, relative to a full-scale input, evaluated
// on `freqs` plus every band centre frequency (narrow high-Q peaks fall between
// grid points). Includes the speaker setup: routing and bass management sum
// channels, so an output can reach the sum of every path into it (the sub takes
// the bass of each small speaker). Per output and frequency this is the most any
// set of full-scale inputs can produce, reached when they are in phase. A muted
// speaker adds nothing, to its own output or to the LFE. Preamp is left out.
double composite_peak_db(const EqState& state, uint32_t channels, uint32_t speaker_mask,
                         const double* freqs, size_t n, double sample_rate);

// The preamp the Auto button sets (plan 4.6): -composite_peak_db, or 0 dB when
// the peak is already below 0 dB. Auto only cuts.
double auto_preamp_db(const EqState& state, uint32_t channels, uint32_t speaker_mask,
                      const double* freqs, size_t n, double sample_rate);

// `count` log-spaced points from `f_lo` to `f_hi` inclusive.
std::vector<double> log_grid(double f_lo, double f_hi, size_t count);

}  // namespace isotone
