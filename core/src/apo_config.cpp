// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/apo_config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "isotone/biquad.h"

namespace isotone {
namespace {

// Upstream's default shelf slope parameter when a config gives no width for a
// shelf: "found out by experimentation with RoomEQWizard" (BiQuadFilterFactory).
constexpr double kDefaultShelfS = 0.9;

// Speaker positions upstream has names for (ChannelHelper's constructor), with
// their ksmedia.h SPEAKER_* bits.
struct NamedPosition {
    uint32_t    bit;
    const char* name;
};
constexpr NamedPosition kNamedPositions[] = {
    {0x1, "L"}, {0x2, "R"}, {0x4, "C"}, {0x8, "LFE"}, {0x10, "RL"},
    {0x20, "RR"}, {0x100, "RC"}, {0x200, "SL"}, {0x400, "SR"},
};

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

bool parse_double(const std::string& s, double* out) { return parse_apo_number(s, out); }

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

// Returns false when the words select no channel. Upstream's ChannelFilter
// starts from an empty selection and adds what it recognises, so the filters
// after such a line act on nothing. Words it cannot use go to `ignored`.
// Upstream's getChannelIndex: a word starting with a digit is a 1-based number
// that must exist in the layout; anything else is looked up by name, with the
// SL/RL, SR/RR and SUB/LFE substitutions. Returns -1 if the word names nothing.
long channel_index(const std::string& upper, const std::vector<std::string>& names) {
    if (!upper.empty() && std::isdigit(static_cast<unsigned char>(upper[0]))) {
        const long n = std::strtol(upper.c_str(), nullptr, 10) - 1;
        return n >= 0 && n < static_cast<long>(names.size()) ? n : -1;
    }
    auto find = [&](const char* name) -> long {
        const auto it = std::find(names.begin(), names.end(), name);
        return it == names.end() ? -1 : static_cast<long>(it - names.begin());
    };
    long index = find(upper.c_str());
    if (index < 0) {
        if (upper == "SL") index = find("RL");
        else if (upper == "SR") index = find("RR");
        else if (upper == "RL") index = find("SL");
        else if (upper == "RR") index = find("SR");
        else if (upper == "SUB") index = find("LFE");
    }
    return index;
}

bool mask_from_channel_words(const std::vector<std::string>& words,
                             const std::vector<std::string>& names, ChannelMask* mask,
                             std::vector<std::string>* ignored) {
    ChannelMask selected = 0;
    for (const std::string& w : words) {
        const std::string upper = to_upper(w);
        if (upper == "ALL") {
            *mask = kAllChannels;
            return true;
        }
        const long index = channel_index(upper, names);
        // A channel past kMaskChannels exists on the device but cannot be named
        // by a mask, so it is reported rather than silently dropped.
        if (index >= 0 && index < static_cast<long>(kMaskChannels)) {
            selected |= ChannelMask{1} << static_cast<uint32_t>(index);
            continue;
        }
        ignored->push_back(w);
    }
    *mask = selected;
    return selected != 0;
}

std::string format_double(double v) { return format_apo_number(v); }

// The width field of a filter line, in a form the parser reads back to the same
// filter. The parser follows upstream: bandwidth is not accepted for shelves
// and a dB slope only for shelves. For those combinations, which only the UI can
// create, the width is written as the Q that designs the same filter; for a
// shelf bandwidth that Q is exact at 48 kHz.
std::string width_text(const Band& b) {
    const bool is_shelf = b.type == FilterType::LowShelf || b.type == FilterType::HighShelf;
    switch (b.width_mode) {
        case WidthMode::Q:
            return " Q " + format_apo_number(b.width);
        case WidthMode::BandwidthOct:
            if (!is_shelf) {
                return " BW Oct " + format_apo_number(b.width);
            } else {
                constexpr double kPi = 3.14159265358979323846;
                const double w0 = 2.0 * kPi * std::min(b.fc, 20000.0) / 48000.0;
                const double alpha = std::sin(w0) * std::sinh(std::log(2.0) / 2.0 * b.width * w0 / std::sin(w0));
                return " Q " + format_apo_number(std::sin(w0) / (2.0 * alpha));
            }
        case WidthMode::SlopeDb:
            // design() reads a slope as Q for everything but shelves.
            return is_shelf ? std::string() : " Q " + format_apo_number(b.width);
    }
    return std::string();
}

std::string mute_copy_line(const std::vector<std::string>& names) {
    std::string line = "Copy:";
    for (const std::string& name : names) {
        line += " " + name + "=0";
    }
    return line;
}

// True for a Copy line that sets every channel of the layout to zero and does
// nothing else: the form format_apo_config writes for mute.
bool copy_is_mute(const std::string& params, const std::vector<std::string>& names) {
    std::istringstream words(params);
    std::string word;
    std::vector<std::string> zeroed;
    while (words >> word) {
        const size_t eq = word.find('=');
        if (eq == std::string::npos || eq == 0) {
            return false;
        }
        double v = 1.0;
        if (!parse_apo_number(word.substr(eq + 1), &v) || v != 0.0 ||
            word.find_first_not_of("0.+-", eq + 1) != std::string::npos) {
            return false;
        }
        zeroed.push_back(to_upper(word.substr(0, eq)));
    }
    if (zeroed.empty() || names.empty()) {
        return false;
    }
    for (const std::string& name : names) {
        if (std::find(zeroed.begin(), zeroed.end(), to_upper(name)) == zeroed.end()) {
            return false;
        }
    }
    return true;
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

uint32_t default_speaker_mask(uint32_t channels) {
    switch (channels) {
        case 1: return 0x4;     // KSAUDIO_SPEAKER_MONO
        case 2: return 0x3;     // KSAUDIO_SPEAKER_STEREO
        case 4: return 0x33;    // KSAUDIO_SPEAKER_QUAD
        case 6: return 0x60F;   // KSAUDIO_SPEAKER_5POINT1_SURROUND
        case 8: return 0x63F;   // KSAUDIO_SPEAKER_7POINT1_SURROUND
        default: return 0;
    }
}

std::vector<std::string> apo_channel_names(const ChannelLayout& layout) {
    std::vector<std::string> names;
    // Upstream walks bits 0 to 30, naming each position present in the mask in
    // order, then numbers whatever channels the mask did not cover.
    for (uint32_t i = 0; i < 31; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        if ((layout.speaker_mask & bit) == 0) {
            continue;
        }
        const char* name = nullptr;
        for (const NamedPosition& p : kNamedPositions) {
            if (p.bit == bit) name = p.name;
        }
        names.push_back(name != nullptr ? name : std::to_string(names.size() + 1));
    }
    while (names.size() < layout.channels) {
        names.push_back(std::to_string(names.size() + 1));
    }
    return names;
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

ApoParseResult parse_apo_config(const std::string& text, const ChannelLayout& layout) {
    ApoParseResult result;

    const std::vector<std::string> channel_names = apo_channel_names(layout);
    ChannelMask current_mask = kAllChannels;
    bool no_channel_selected = false;
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
            std::vector<std::string> ignored;
            no_channel_selected =
                !mask_from_channel_words(words, channel_names, &current_mask, &ignored);
            for (const std::string& word : ignored) {
                result.warnings.push_back({line_no, "unknown channel '" + word + "', ignored"});
            }
            continue;
        }
        if (command == "Preamp") {
            // "Preamp: -6.1 dB": upstream scans a leading double and ignores the
            // unit text after it. Each line is its own gain stage on the channels
            // selected at that point, so lines add, and under a Channel scope
            // the gain is a per-channel trim.
            double v = 0.0;
            if (!parse_double(trim(normalise_decimal(params)), &v)) {
                result.warnings.push_back({line_no, "could not read preamp value"});
            } else if (no_channel_selected) {
                result.warnings.push_back(
                    {line_no, "preamp ignored: the Channel line before it selects no channel"});
            } else if (current_mask == kAllChannels) {
                result.state.preamp_db += v;
            } else {
                for (uint32_t c = 0; c < kMaskChannels; ++c) {
                    if ((current_mask & (ChannelMask{1} << c)) == 0) {
                        continue;
                    }
                    if (c < kMaxChannels) {
                        result.state.channel_gain_db[c] += v;
                    } else {
                        result.warnings.push_back(
                            {line_no, "preamp for channel " + std::to_string(c + 1) +
                                          " ignored: only the first " +
                                          std::to_string(kMaxChannels) + " channels have a trim"});
                    }
                }
            }
            continue;
        }
        if (command == "Copy" && copy_is_mute(params, channel_names)) {
            result.state.mute = true;
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
            if (no_channel_selected) {
                result.warnings.push_back(
                    {line_no, "filter ignored: the Channel line before it selects no channel"});
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

std::string format_apo_number(double v) {
    char buf[64];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 12);
    return std::string(buf, r.ec == std::errc() ? r.ptr : buf);
}

std::string format_apo_frequency(double hz) {
    std::string s = format_apo_number(hz);
    if (s.size() >= 5 && s.find_first_of("eE") == std::string::npos && s[s.size() - 4] == '.') {
        s += '0';
    }
    return s;
}

bool parse_apo_number(const std::string& s, double* out) {
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    if (begin < end && *begin == '+') {
        ++begin;
    }
    double v = 0.0;
    const std::from_chars_result r = std::from_chars(begin, end, v, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr == begin || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

std::string format_apo_config(const EqState& state, const ApoFormatOptions& options) {
    std::ostringstream out;

    // A channel the layout does not have is still written, by number, so the
    // band is not lost; Equalizer APO reports it as out of range on that device.
    const std::vector<std::string> channel_names = apo_channel_names(options.layout);
    const auto fc_text = [&](const Band& b) {
        return format_apo_frequency(options.sample_rate > 0.0 ? clamp_fc(b.fc, options.sample_rate) : b.fc);
    };
    const auto channel_name = [&](uint32_t c) {
        return c < channel_names.size() ? channel_names[c] : std::to_string(c + 1);
    };

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
            for (uint32_t c = 0; c < kMaskChannels; ++c) {
                if ((mask & (ChannelMask{1} << c)) != 0) {
                    out << " " << channel_name(c);
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
                        << " Fc " << fc_text(b) << " Hz";
                    if (type_uses_gain(b.type)) {
                        out << " Gain " << format_double(b.gain_db) << " dB";
                    }
                    out << width_text(b) << "\n";
                }
                continue;
            }

            const bool is_shelf = b.type == FilterType::LowShelf || b.type == FilterType::HighShelf;
            out << "Filter " << index++ << ": ON " << token_for(b.type, b.shelf_corner) << " ";
            if (b.width_mode == WidthMode::SlopeDb && is_shelf) {
                out << format_double(b.width) << " dB ";
            }
            out << "Fc " << fc_text(b) << " Hz";
            if (type_uses_gain(b.type)) {
                out << " Gain " << format_double(b.gain_db) << " dB";
            }
            out << width_text(b) << "\n";
        }
    }

    // Channel trims ride on Preamp inside a Channel block, which is how Peace
    // expresses them too.
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (state.channel_gain_db[c] == 0.0) {
            continue;
        }
        out << "Channel: " << channel_name(c) << "\n";
        out << "Preamp: " << format_double(state.channel_gain_db[c]) << " dB\n";
    }

    // Mute is silence, not a large cut: every channel of the layout is copied
    // from nothing. The parser reads this exact form back as mute.
    if (state.mute) {
        out << "Channel: all\n";
        out << mute_copy_line(channel_names) << "\n";
    }

    return out.str();
}

}  // namespace isotone
