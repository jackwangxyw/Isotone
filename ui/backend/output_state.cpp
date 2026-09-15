// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "output_state.h"

#include <algorithm>
#include <cmath>

namespace isotone::ui {

void apply_balance(double balance, uint32_t channels, EqState* state) {
    if (channels != 2 || !std::isfinite(balance)) return;
    const double b = std::clamp(balance, -1.0, 1.0);
    const uint32_t quiet = b < 0 ? 1u : 0u;   // the side turned down: right when balance favours left
    state->channel_gain_db[0] = 0.0;
    state->channel_gain_db[1] = 0.0;
    state->speakers.muted &= ~ChannelMask{0x3};
    if (b == 0.0) return;
    if (std::abs(b) >= 1.0) {
        state->speakers.muted |= ChannelMask{1} << quiet;
    } else {
        state->channel_gain_db[quiet] = 20.0 * std::log10(1.0 - std::abs(b));
    }
}

double balance_from_state(const EqState& state, uint32_t channels) {
    if (channels != 2) return 0.0;
    const ChannelMask muted = state.speakers.muted & 0x3;
    if (muted == 0x2) return -1.0;
    if (muted == 0x1) return 1.0;
    const double left = state.channel_gain_db[0], right = state.channel_gain_db[1];
    if (left == 0.0 && right < 0.0) return -(1.0 - std::pow(10.0, right / 20.0));
    if (right == 0.0 && left < 0.0) return 1.0 - std::pow(10.0, left / 20.0);
    return 0.0;
}

void clear_balance(uint32_t channels, EqState* state) { apply_balance(0.0, channels, state); }

EqState state_for_output(const EqState& edited, double balance, const OutputLayout& layout) {
    EqState s = edited;
    s.layout_channels = layout.channels;
    s.layout_speaker_mask = layout.speaker_mask;
    apply_balance(balance, layout.channels, &s);
    return s;
}

}  // namespace isotone::ui
