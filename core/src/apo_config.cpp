// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/apo_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace isotone {
namespace {

// Upstream's default shelf slope parameter when a config gives no width for a
// shelf: "found out by experimentation with RoomEQWizard" (BiQuadFilterFactory).
constexpr double kDefaultShelfS = 0.9;

const char* const kChannelNames[] = {"L", "R", "C", "LFE", "RL", "RR", "SL", "SR"};

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string to_upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// Upstream normalises a comma decimal mark to a period before parsing, so a
// config written on a European locale machine loads correctly.
std::string normalise_decimal(std::string s) {
    std::replace(s.begin(), s.end(), ',', '.');
    return s;
}

bool parse_double(const std::string& s, double* out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

// Room EQ Wizard writes a thousands separator as a period, so "1.000" means
// 1000 Hz rather than 1 Hz. Upstream's rule, reproduced exactly: a string of at
// least five characters, no exponent, with a period four characters from the
// end, is multiplied by a thousand.
double apply_rew_thousands_quirk(const std::string& raw, double value) {
    if (raw.size() < 5) {
        return value;
    }
    if (raw.find_first_of("eE") != std::string::npos) {
        return value;
    }
    if (raw[raw.size() - 4] == '.') {
        return value * 1000.0;
    }
    return value;
}

struct TokenInfo {
    FilterType type;
    bool corner_freq;   // true for the tokens that are not spelled with a C
};

bool lookup_token(const std::string& token, TokenInfo* out) {
    static const struct { const char* name; FilterType type; bool corner; } kMap[] = {
        {"PK",    FilterType::Peaking,   false},
        {"PEQ",   FilterType::Peaking,   false},
        {"MODAL", FilterType::Peaking,   false},
        {"LP",    FilterType::LowPass,   false},
        {"LPQ",   FilterType::LowPass,   false},
        {"HP",    FilterType::HighPass,  false},
        {"HPQ",   FilterType::HighPass,  false},
        {"BP",    FilterType::BandPass,  false},
        {"NO",    FilterType::Notch,     false},
        {"AP",    FilterType::AllPass,   false},
        {"LS",    FilterType::LowShelf,  true},
        {"HS",    FilterType::HighShelf, true},
        {"LSC",   FilterType::LowShelf,  false},
        {"HSC",   FilterType::HighShelf, false},
    };
    const std::string upper = to_upper(token);
    for (const auto& e : kMap) {
        if (upper == e.name) {
            out->type = e.type;
            out->corner_freq = e.corner;
            return true;
        }
    }
    return false;
}

// Finds `key` as a whole word and returns the numeric run that follows it.
// Mirrors upstream's regex searches, which are position-independent within the
// parameter text rather than requiring a fixed field order.
bool find_value_after(const std::string& text, const std::string& key, std::string* raw,
                      size_t* found_at = nullptr) {
    const std::string upper = to_upper(text);
    const std::string ukey  = to_upper(key);
    size_t pos = 0;
    while ((pos = upper.find(ukey, pos)) != std::string::npos) {
        const bool space_before = pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1]));
        size_t after = pos + ukey.size();
        if (!space_before) {
            pos = after;
            continue;
        }
        while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) {
            ++after;
        }
        const size_t start = after;
        while (after < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[after])) || text[after] == '.' ||
                text[after] == '-' || text[after] == '+' || text[after] == 'e' ||
                text[after] == 'E')) {
            ++after;
        }
        if (after > start) {
            *raw = text.substr(start, after - start);
            if (found_at != nullptr) {
                *found_at = pos;
            }
            return true;
        }
        pos = after;
    }
    return false;
}

ChannelMask mask_from_channel_words(const std::vector<std::string>& words, bool* all) {
    *all = false;
    ChannelMask mask = 0;
    for (const std::string& w : words) {
        const std::string upper = to_upper(w);
        if (upper == "ALL") {
            *all = true;
            return kAllChannels;
        }
        uint32_t index = 0;
        if (apo_channel_index(upper, &index)) {
            mask |= ChannelMask{1} << index;
            continue;
        }
        // A bare number is a 1-based channel index.
        char* end = nullptr;
        const long n = std::strtol(upper.c_str(), &end, 10);
        if (end != upper.c_str() && *end == '\0' && n >= 1 && n <= static_cast<long>(kMaxChannels)) {
            mask |= ChannelMask{1} << static_cast<uint32_t>(n - 1);
        }
    }
    if (mask == 0) {
        *all = true;
        return kAllChannels;
    }
    return mask;
}

// 12 significant digits. The default %g gives 6, which silently rounds a
// frequency like 1419.857 Hz to 1419.86 and loses the round trip. Short values
// still print short, so a hand-written "Q 0.7" comes back out as "Q 0.7".
std::string format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return buf;
}

const char* token_for(FilterType type, bool corner) {
    switch (type) {
        case FilterType::Peaking:   return "PK";
        case FilterType::LowPass:   return "LPQ";
        case FilterType::HighPass:  return "HPQ";
        case FilterType::BandPass:  return "BP";
        case FilterType::Notch:     return "NO";
        case FilterType::AllPass:   return "AP";
        case FilterType::LowShelf:  return corner ? "LS" : "LSC";
        case FilterType::HighShelf: return corner ? "HS" : "HSC";
    }
    return "PK";
}

bool type_uses_gain(FilterType t) {
    return t == FilterType::Peaking || t == FilterType::LowShelf || t == FilterType::HighShelf;
}

}  // namespace

const char* apo_channel_name(uint32_t channel) {
    if (channel < sizeof(kChannelNames) / sizeof(kChannelNames[0])) {
        return kChannelNames[channel];
    }
    return "";
}

bool apo_channel_index(const std::string& name, uint32_t* out) {
    const std::string upper = to_upper(name);
    for (uint32_t i = 0; i < sizeof(kChannelNames) / sizeof(kChannelNames[0]); ++i) {
        if (upper == kChannelNames[i]) {
            *out = i;
            return true;
        }
    }
    if (upper == "SUB") {   // old name for LFE
        *out = 3;
        return true;
    }
    return false;
}

std::string apo_device_pattern_for_guid(const std::string& guid) {
    std::string g = trim(guid);
    if (g.empty()) {
        return {};
    }
    if (g.front() != '{') {
        g = "{" + g;
    }
    if (g.back() != '}') {
        g += "}";
    }
    return g;
}

ApoParseResult parse_apo_config(const std::string& text) {
    ApoParseResult result;

    ChannelMask current_mask = kAllChannels;
    uint32_t next_id = 1;
    size_t line_no = 0;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        ++line_no;

        // Upstream strips a trailing comment starting at '#'.
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            result.warnings.push_back({line_no, "no ':' in line, ignored"});
            continue;
        }
        const std::string command = trim(line.substr(0, colon));
        const std::string params  = line.substr(colon + 1);

        if (command == "Device") {
            result.devices.push_back(trim(params));
            continue;
        }
        if (command == "Channel") {
            std::vector<std::string> words;
            std::istringstream ws(params);
            std::string w;
            while (ws >> w) {
                words.push_back(w);
            }
            bool all = false;
            current_mask = mask_from_channel_words(words, &all);
            continue;
        }
        if (command == "Preamp") {
            // "Preamp: -6.1 dB": upstream scans a leading double and ignores the
            // unit text after it.
            double v = 0.0;
            if (parse_double(trim(normalise_decimal(params)), &v)) {
                result.state.preamp_db = v;
            } else {
                result.warnings.push_back({line_no, "could not read preamp value"});
            }
            continue;
        }
        if (command.rfind("Filter", 0) == 0) {
            const std::string norm = normalise_decimal(params);
            std::istringstream ps(norm);
            std::string on, type_token;
            ps >> on;
            if (to_upper(on) != "ON") {
                // "Filter 3: OFF ..." or "Filter 3: None" are both legal ways to
                // say nothing is here.
                continue;
            }
            ps >> type_token;
            if (to_upper(type_token) == "NONE") {
                continue;
            }
            TokenInfo info{};
            if (!lookup_token(type_token, &info)) {
                result.warnings.push_back({line_no, "unknown filter type '" + type_token + "'"});
                continue;
            }

            // Everything after the type token, which is what upstream's regexes
            // are applied to.
            std::string rest;
            std::getline(ps, rest);

            Band band;
            band.id = next_id++;
            band.type = info.type;
            band.channels = current_mask;
            band.enabled = true;

            std::string raw;
            if (!find_value_after(rest, "Fc", &raw)) {
                result.warnings.push_back({line_no, "no Fc in filter line"});
                continue;
            }
            double fc = 0.0;
            if (!parse_double(raw, &fc)) {
                result.warnings.push_back({line_no, "could not read Fc"});
                continue;
            }
            band.fc = apply_rew_thousands_quirk(raw, fc);

            if (find_value_after(rest, "Gain", &raw)) {
                double g = 0.0;
                if (parse_double(raw, &g) && type_uses_gain(info.type)) {
                    band.gain_db = g;
                }
            } else if (type_uses_gain(info.type)) {
                result.warnings.push_back({line_no, "no Gain for a filter type that needs one"});
                continue;
            }

            const bool is_shelf =
                info.type == FilterType::LowShelf || info.type == FilterType::HighShelf;

            bool have_width = false;
            if (find_value_after(rest, "BW Oct", &raw)) {
                double bw = 0.0;
                if (parse_double(raw, &bw) && bw > 0.0 && !is_shelf) {
                    band.width = bw;
                    band.width_mode = WidthMode::BandwidthOct;
                    have_width = true;
                }
            }
            if (!have_width && find_value_after(rest, "Q", &raw)) {
                double q = 0.0;
                if (parse_double(raw, &q) && q > 0.0) {
                    band.width = q;
                    band.width_mode = WidthMode::Q;
                    have_width = true;
                }
            }
            if (!have_width && is_shelf) {
                // A slope appears immediately after the type token, as
                // "Filter 1: ON LS 6 dB Fc 100 Hz".
                const std::string lead = trim(rest);
                double slope = 0.0;
                std::string num;
                size_t i = 0;
                while (i < lead.size() && (std::isdigit(static_cast<unsigned char>(lead[i])) ||
                                           lead[i] == '.' || lead[i] == '-' || lead[i] == '+')) {
                    num += lead[i++];
                }
                while (i < lead.size() && std::isspace(static_cast<unsigned char>(lead[i]))) {
                    ++i;
                }
                if (!num.empty() && lead.compare(i, 2, "dB") == 0 && parse_double(num, &slope) &&
                    slope > 0.0) {
                    band.width = slope;
                    band.width_mode = WidthMode::SlopeDb;
                    have_width = true;
                }
            }

            if (!have_width) {
                // Upstream's defaults when the width field is absent.
                switch (info.type) {
                    case FilterType::LowPass:
                    case FilterType::HighPass:
                    case FilterType::BandPass:
                        band.width = 0.70710678118654752;
                        band.width_mode = WidthMode::Q;
                        break;
                    case FilterType::Notch:
                        band.width = 30.0;
                        band.width_mode = WidthMode::Q;
                        break;
                    case FilterType::LowShelf:
                    case FilterType::HighShelf:
                        // Upstream sets S = 0.9 and, crucially, does NOT then
                        // divide by 12 and does NOT set the corner flag: both of
                        // those live in an `else if` that only runs when a width
                        // was actually given. Expressed here as a dB slope
                        // because design() divides by 12, so 10.8 dB is S = 0.9.
                        band.width = kDefaultShelfS * 12.0;
                        band.width_mode = WidthMode::SlopeDb;
                        break;
                    case FilterType::Peaking:
                    case FilterType::AllPass:
                        result.warnings.push_back(
                            {line_no, "no Q or bandwidth for a filter type that needs one"});
                        continue;
                }
            }

            // The corner-frequency correction applies only to shelves, only to
            // the tokens spelled without a trailing C, and only when a width was
            // given explicitly. A bare `HS Fc 1100 Hz Gain -3 dB` gets no
            // correction at all. Verified by measuring a real config through a
            // real Equalizer APO install.
            band.shelf_corner = is_shelf && info.corner_freq && have_width;

            result.state.bands.push_back(band);
            continue;
        }

        // Everything else is preserved but not modelled.
        result.unsupported.push_back(line);
    }

    return result;
}

std::string format_apo_config(const EqState& state, const ApoFormatOptions& options) {
    std::ostringstream out;

    if (!options.header_comment.empty()) {
        std::istringstream hs(options.header_comment);
        std::string h;
        while (std::getline(hs, h)) {
            out << "# " << h << "\n";
        }
    }
    if (!options.device.empty()) {
        out << "Device: " << options.device << "\n";
    }

    if (state.preamp_db != 0.0) {
        out << "Preamp: " << format_double(state.preamp_db) << " dB\n";
    }

    // Group bands by channel mask so a Channel: line is written once per group
    // rather than once per band.
    std::vector<ChannelMask> masks;
    for (const Band& b : state.bands) {
        if (std::find(masks.begin(), masks.end(), b.channels) == masks.end()) {
            masks.push_back(b.channels);
        }
    }

    int index = 1;
    for (ChannelMask mask : masks) {
        if (mask == kAllChannels) {
            if (masks.size() > 1) {
                out << "Channel: all\n";
            }
        } else {
            out << "Channel:";
            for (uint32_t c = 0; c < kMaxChannels; ++c) {
                if ((mask & (ChannelMask{1} << c)) != 0) {
                    out << " " << apo_channel_name(c);
                }
            }
            out << "\n";
        }

        for (const Band& b : state.bands) {
            if (b.channels != mask) {
                continue;
            }
            if (!b.enabled) {
                if (options.write_disabled_as_none) {
                    out << "Filter " << index++ << ": OFF " << token_for(b.type, b.shelf_corner)
                        << " Fc " << format_double(b.fc) << " Hz";
                    if (type_uses_gain(b.type)) {
                        out << " Gain " << format_double(b.gain_db) << " dB";
                    }
                    if (b.width_mode == WidthMode::Q) {
                        out << " Q " << format_double(b.width);
                    } else if (b.width_mode == WidthMode::BandwidthOct) {
                        out << " BW Oct " << format_double(b.width);
                    }
                    out << "\n";
                }
                continue;
            }

            out << "Filter " << index++ << ": ON " << token_for(b.type, b.shelf_corner) << " ";
            if (b.width_mode == WidthMode::SlopeDb) {
                out << format_double(b.width) << " dB ";
            }
            out << "Fc " << format_double(b.fc) << " Hz";
            if (type_uses_gain(b.type)) {
                out << " Gain " << format_double(b.gain_db) << " dB";
            }
            if (b.width_mode == WidthMode::Q) {
                out << " Q " << format_double(b.width);
            } else if (b.width_mode == WidthMode::BandwidthOct) {
                out << " BW Oct " << format_double(b.width);
            }
            out << "\n";
        }
    }

    // Channel trims and mute ride on Preamp inside a Channel block, which is how
    // Peace expresses them too.
    bool any_trim = state.mute;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (state.channel_gain_db[c] != 0.0) {
            any_trim = true;
        }
    }
    if (any_trim) {
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            const double db = state.mute ? -100.0 : state.channel_gain_db[c];
            if (db == 0.0) {
                continue;
            }
            out << "Channel: " << apo_channel_name(c) << "\n";
            out << "Preamp: " << format_double(db) << " dB\n";
        }
    }

    return out.str();
}

}  // namespace isotone
