// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The EqState the engine gets for an output: the edited state with the output's
// layout set and the Channels panel's balance written as channel trims and
// speaker mute (docs/ui-spec.md, "Channels panel").

#pragma once

#include <cstdint>

#include "isotone/types.h"

namespace isotone::ui {

struct OutputLayout {
    uint32_t channels = 2;
    uint32_t speaker_mask = 0x3;
    double sample_rate = 48000.0;
};

// Balance b in [-1, 1]: only the opposite side is turned down, linearly, to a
// gain of 1 - |b|; at |b| = 1 it is muted with the speaker mute bit, since a trim
// cannot reach silence. The favoured side is untouched. Stereo outputs only
// (channel 0 left, 1 right); on other layouts balance is not written.
void apply_balance(double balance, uint32_t channels, EqState* state);

// The balance a state's trims and speaker mute express, as apply_balance writes
// them; 0 for anything else. clear_balance removes what apply_balance wrote, so
// the edited state holds balance only as the Channels panel's value.
double balance_from_state(const EqState& state, uint32_t channels);
void clear_balance(uint32_t channels, EqState* state);

// `edited` for `layout`, with balance applied.
EqState state_for_output(const EqState& edited, double balance, const OutputLayout& layout);

}  // namespace isotone::ui
