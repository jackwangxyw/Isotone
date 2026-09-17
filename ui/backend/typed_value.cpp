// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "typed_value.h"

#include <charconv>
#include <cmath>
#include <string>

namespace isotone::ui {

namespace {

std::string normalised(std::string_view text) {
    std::string s;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.substr(i, 3) == "\xE2\x88\x92") {   // U+2212 MINUS SIGN
            s += '-';
            i += 2;
        } else if (text[i] == ',') {
            s += '.';
        } else {
            const char c = text[i];
            s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    return s;
}

void skip_spaces(std::string_view& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
}

// A sign, digits and at most one decimal point: no exponent, hex, inf or nan.
std::optional<double> take_number(std::string_view& s) {
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    const size_t start = i;
    bool digits = false, point = false;
    for (; i < s.size(); ++i) {
        if (s[i] >= '0' && s[i] <= '9') {
            digits = true;
        } else if (s[i] == '.' && !point) {
            point = true;
        } else {
            break;
        }
    }
    if (!digits) return std::nullopt;
    double v = 0.0;
    const auto [end, ec] = std::from_chars(s.data() + start, s.data() + i, v);
    if (ec != std::errc{} || end != s.data() + i || !std::isfinite(v)) return std::nullopt;
    s.remove_prefix(i);
    return negative ? -v : v;
}

}  // namespace

std::optional<double> parse_typed_value(std::string_view text, TypedUnit unit) {
    const std::string buffer = normalised(text);
    std::string_view s = buffer;
    skip_spaces(s);
    if (unit == TypedUnit::Q && !s.empty() && s.front() == 'q') {
        s.remove_prefix(1);
        skip_spaces(s);
    }
    std::optional<double> v = take_number(s);
    if (!v) return std::nullopt;
    skip_spaces(s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);

    double scale = 1.0;
    bool unit_ok = s.empty();
    switch (unit) {
        case TypedUnit::Decibels: unit_ok = unit_ok || s == "db"; break;
        case TypedUnit::Hertz:
            if (s == "k" || s == "khz") {
                scale = 1000.0;
                unit_ok = true;
            }
            unit_ok = unit_ok || s == "hz";
            break;
        case TypedUnit::Q: break;
        case TypedUnit::Octaves: unit_ok = unit_ok || s == "oct"; break;
        case TypedUnit::SlopeDb: unit_ok = unit_ok || s == "db" || s == "db/oct"; break;
        case TypedUnit::Plain: break;
        case TypedUnit::Metres: unit_ok = unit_ok || s == "m"; break;
        case TypedUnit::Milliseconds: unit_ok = unit_ok || s == "ms"; break;
        case TypedUnit::Dbfs: unit_ok = unit_ok || s == "dbfs" || s == "db"; break;
        case TypedUnit::OctavesPerSecond: unit_ok = unit_ok || s == "oct/s"; break;
    }
    if (!unit_ok) return std::nullopt;
    const double result = *v * scale;
    if (!std::isfinite(result)) return std::nullopt;
    return result;
}

}  // namespace isotone::ui
