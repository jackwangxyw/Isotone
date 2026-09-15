// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// A value typed into a click-to-edit field: a number, optionally in the field's
// own unit, as the field shows it ("−3.0 dB", "1.20 kHz", "Q 1.41", "1.50 oct",
// "12.0 dB/oct") or shorter ("-3", "1.2k", "1200 hz").

#pragma once

#include <optional>
#include <string_view>

namespace isotone::ui {

enum class TypedUnit {
    Decibels,
    Hertz,      // "k" and "kHz" multiply by 1000
    Q,          // written before the number, as the column shows it
    Octaves,
    SlopeDb,    // "dB/oct"
    Plain,      // no unit: balance
};

// The value in the field's base unit (dB, Hz, Q, octaves, dB per octave), or
// nothing when the text is not a finite number in that unit. UTF-8; U+2212 is a
// minus, and a comma is a decimal point.
std::optional<double> parse_typed_value(std::string_view text, TypedUnit unit);

}  // namespace isotone::ui
