// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Values typed into the click-to-edit fields.

#include "doctest.h"

#include "typed_value.h"

using namespace isotone::ui;

namespace {

bool reads(const char* text, TypedUnit unit, double expected) {
    const auto v = parse_typed_value(text, unit);
    return v.has_value() && *v == doctest::Approx(expected).epsilon(1e-12);
}

bool refused(const char* text, TypedUnit unit) { return !parse_typed_value(text, unit).has_value(); }

}  // namespace

TEST_CASE("a typed value reads as the field shows it") {
    CHECK(reads("+3.0 dB", TypedUnit::Decibels, 3.0));
    CHECK(reads("\xE2\x88\x92" "3.0 dB", TypedUnit::Decibels, -3.0));   // U+2212, as every label writes it
    CHECK(reads("1.00 kHz", TypedUnit::Hertz, 1000.0));
    CHECK(reads("250 Hz", TypedUnit::Hertz, 250.0));
    CHECK(reads("Q 1.41", TypedUnit::Q, 1.41));
    CHECK(reads("1.50 oct", TypedUnit::Octaves, 1.5));
    CHECK(reads("12.0 dB/oct", TypedUnit::SlopeDb, 12.0));
    CHECK(reads("\xE2\x88\x92" "0.5", TypedUnit::Plain, -0.5));
}

TEST_CASE("a typed value reads shorter, in any case and spacing") {
    CHECK(reads("-3", TypedUnit::Decibels, -3.0));
    CHECK(reads("  4.5db ", TypedUnit::Decibels, 4.5));
    CHECK(reads("1.2k", TypedUnit::Hertz, 1200.0));
    CHECK(reads("1.2 KHZ", TypedUnit::Hertz, 1200.0));
    CHECK(reads("80hz", TypedUnit::Hertz, 80.0));
    CHECK(reads("15000", TypedUnit::Hertz, 15000.0));
    CHECK(reads("2", TypedUnit::Q, 2.0));
    CHECK(reads("q2", TypedUnit::Q, 2.0));
    CHECK(reads("0.7", TypedUnit::Octaves, 0.7));
    CHECK(reads("6 db", TypedUnit::SlopeDb, 6.0));
    CHECK(reads(".5", TypedUnit::Q, 0.5));
    CHECK(reads("1,5", TypedUnit::Octaves, 1.5));
    CHECK(reads("+0.3", TypedUnit::Plain, 0.3));
}

TEST_CASE("a typed value that is not a number in the field's unit is refused") {
    CHECK(refused("", TypedUnit::Decibels));
    CHECK(refused("   ", TypedUnit::Decibels));
    CHECK(refused("dB", TypedUnit::Decibels));
    CHECK(refused("-", TypedUnit::Decibels));
    CHECK(refused("3 Hz", TypedUnit::Decibels));
    CHECK(refused("1k", TypedUnit::Decibels));
    CHECK(refused("3 dB", TypedUnit::Hertz));
    CHECK(refused("1.2 kk", TypedUnit::Hertz));
    CHECK(refused("1.50 oct", TypedUnit::Q));
    CHECK(refused("Q 1.5", TypedUnit::Octaves));
    CHECK(refused("2 Q", TypedUnit::Q));
    CHECK(refused("12 dB", TypedUnit::Plain));
    CHECK(refused("1.2.3", TypedUnit::Plain));
    CHECK(refused("1e999", TypedUnit::Plain));
    CHECK(refused("1e3", TypedUnit::Hertz));
    CHECK(refused("nan", TypedUnit::Plain));
    CHECK(refused("inf", TypedUnit::Plain));
    CHECK(refused("0x10", TypedUnit::Plain));
    CHECK(refused("--3", TypedUnit::Decibels));
    CHECK(refused("3-", TypedUnit::Decibels));
    CHECK(refused("1,5,0", TypedUnit::Plain));
    CHECK(refused("1.5,0", TypedUnit::Plain));
}
