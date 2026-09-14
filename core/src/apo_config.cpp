// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/apo_config.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>

#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"

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

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Upstream splits Channel and Stage values on spaces only; a tab is part of a word.
std::vector<std::string> split_spaces(const std::string& s) {
    std::vector<std::string> words;
    std::string word;
    for (char c : s + " ") {
        if (c != ' ') {
            word += c;
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    return words;
}

// Upstream normalises a comma decimal mark to a period before parsing, so a
// config written on a European locale machine loads correctly.
std::string normalise_decimal(std::string s) {
    std::replace(s.begin(), s.end(), ',', '.');
    return s;
}

bool parse_double(const std::string& s, double* out) { return parse_apo_number(s, out); }

// Upstream reads widths and gains with wcstod, which gives 0 for no number.
double number_or_zero(const std::string& s) {
    double v = 0.0;
    return parse_double(s, &v) ? v : 0.0;
}

// Removes the non-breaking spaces a locale writes as a thousands separator:
// U+00A0 as UTF-8, or the lone byte upstream's code-page fallback reads as it.
std::string strip_nbsp(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\xC2' && i + 1 < s.size() && s[i + 1] == '\xA0') {
            ++i;
        } else if (s[i] != '\xA0') {
            out += s[i];
        }
    }
    return out;
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
        {"Modal", FilterType::Peaking,   false},
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
    // Case-sensitive, as upstream's filterNameToTypeMap is.
    for (const auto& e : kMap) {
        if (token == e.name) {
            out->type = e.type;
            out->corner_freq = e.corner;
            return true;
        }
    }
    return false;
}

// Upstream's filter-line patterns (BiQuadFilterFactory.cpp), unchanged but for
// U+00A0 in Fc, which is matched as either of its byte forms: case-sensitive,
// units required, searched anywhere in the text after the type.
struct FilterPatterns {
    std::regex type{R"(^\s*ON\s+([A-Za-z]+))"};
    std::regex off{R"(^\s*OFF\s+([A-Za-z]+))"};
    std::regex freq{"\\s+Fc\\s*((?:[-+0-9.eE]|\xC2\xA0|\xA0)+)\\s*H\\s*z"};
    std::regex gain{R"(\s+Gain\s*([-+0-9.eE]+)\s*dB)"};
    std::regex q{R"(\s+Q\s*([-+0-9.eE]+))"};
    std::regex bw{R"(\s+BW\s+Oct\s*([-+0-9.eE]+))"};
    std::regex slope{R"(^\s*([-+0-9.eE]+)\s*dB)"};
};

const FilterPatterns& filter_patterns() {
    static const FilterPatterns patterns;
    return patterns;
}

// Each run of whitespace as one space. The patterns above match whitespace only
// with \s+ and \s*, and nothing else in them matches it, so a run is always
// consumed whole and one space matches wherever a longer run does, with the
// same captures. It keeps matching linear: std::regex backtracks through a run
// once for every position in it (50,000 spaces took 78 s with libstdc++, and
// Visual C++ threw error_complexity at 600).
std::string collapse_whitespace(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        if (!space) {
            out += c;
        } else if (out.empty() || out.back() != ' ') {
            out += ' ';
        }
    }
    return out;
}

// The longest filter line matched, after whitespace is collapsed. A real one is
// under 100 characters. libstdc++ matches a repeated group recursively, one
// stack frame per character, and a line of a million digits overflowed the
// stack instead of throwing; upstream's Visual C++ regex gives up on such a line
// as well, so it is skipped either way.
constexpr size_t kMaxFilterLineChars = 1024;

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

// A band's gain as the processor uses it.
double written_gain(const Band& b) { return std::clamp(b.gain_db, -kMaxBandGainDb, kMaxBandGainDb); }

// The steepest dB slope a shelf of this gain can have and stay the filter the
// processor designs: design() holds the term under the square root at 0.01,
// and upstream, which does not, takes the root of a negative number past it.
double written_slope(const Band& b) {
    const double A = std::pow(10.0, written_gain(b) / 40.0);
    const double steepest = 12.0 / (1.0 - 1.99 / (A + 1.0 / A));   // inner = 0.01
    return std::min(b.width, steepest);
}

// The width field of a filter line, in a form the parser reads back to the same
// filter. The parser follows upstream: bandwidth is not accepted for shelves
// and a dB slope only for shelves. For those combinations, which only the UI can
// create, the width is written as the Q that designs the same filter; for a
// shelf bandwidth that Q is exact at `sample_rate`.
std::string width_text(const Band& b, double sample_rate) {
    const bool is_shelf = b.type == FilterType::LowShelf || b.type == FilterType::HighShelf;
    switch (b.width_mode) {
        case WidthMode::Q:
            return " Q " + format_apo_number(b.width);
        case WidthMode::BandwidthOct:
            if (!is_shelf) {
                return " BW Oct " + format_apo_number(b.width);
            } else {
                constexpr double kPi = 3.14159265358979323846;
                const double w0 = 2.0 * kPi * clamp_fc(b.fc, sample_rate) / sample_rate;
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

// Upstream's StringHelper::split: the parts between `sep`, empty ones dropped.
std::vector<std::string> split_on(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at = s.find(sep); ; at = s.find(sep, from)) {
        const std::string part = s.substr(from, at == std::string::npos ? std::string::npos : at - from);
        if (!part.empty()) parts.push_back(part);
        if (at == std::string::npos) return parts;
        from = at + 1;
    }
}

// A number as wcstod reads it, which is how upstream reads a Copy factor:
// leading whitespace, one sign, then a decimal or 0x hexadecimal number, inf or
// nan; 0 where there is no number. Past double's range it is 0 or infinity.
double read_like_wcstod(const std::string& s) {
    const char* p = s.data();
    const char* const end = s.data() + s.size();
    while (p < end && (*p == ' ' || (*p >= '\t' && *p <= '\r'))) ++p;
    const bool negative = p < end && *p == '-';
    if (p < end && (*p == '+' || *p == '-')) ++p;
    const bool hex = end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    const char* const digits = hex ? p + 2 : p;
    if (digits < end && (*digits == '+' || *digits == '-')) {
        return 0.0;   // a second sign, which from_chars would take: no number, or "0x" read as 0
    }
    double v = 0.0;
    const std::from_chars_result r =
        std::from_chars(digits, end, v, hex ? std::chars_format::hex : std::chars_format::general);
    if (r.ptr == digits) {
        return 0.0;   // no number; for "0x" alone, wcstod reads the 0
    }
    if (r.ec == std::errc::result_out_of_range) {
        // The order of magnitude decides which: where the first nonzero digit
        // is, plus the exponent. Hexadecimal digits are 4 bits and the exponent
        // is in bits.
        const char mark = hex ? 'p' : 'e';
        long long order = 0;
        bool point = false, significant = false;
        const char* q = digits;
        for (; q < r.ptr && std::tolower(static_cast<unsigned char>(*q)) != mark; ++q) {
            if (*q == '.') {
                point = true;
            } else if (significant) {
                order += point ? 0 : 1;
            } else if (*q != '0') {
                significant = true;
            } else if (point) {
                --order;
            }
        }
        long long exponent = 0;
        if (q + 1 < r.ptr) {
            const char* e = q[1] == '+' ? q + 2 : q + 1;
            if (e < r.ptr && std::from_chars(e, r.ptr, exponent).ec != std::errc()) {
                exponent = *e == '-' ? -(1LL << 40) : (1LL << 40);
            }
        }
        v = (hex ? order * 4 : order) + exponent < 0 ? 0.0 : std::numeric_limits<double>::infinity();
    }
    return negative ? -v : v;
}

// True for a Copy line that sets every channel of the layout to zero and does
// nothing else, read as upstream reads it (CopyFilterFactory::createFilter,
// CopyFilter::initialize): words split on spaces, each target=source; a source
// is summands split on '+', each factor*channel, factor or channel. A lone
// summand is a factor only if it is "0" or has a period; otherwise it names a
// channel, and a name that resolves to nothing is added as a constant 1.0. A
// factor ending in dB is a level. The float factor must be 0.
bool copy_is_mute(const std::string& params, const std::vector<std::string>& names) {
    std::vector<bool> zeroed(names.size(), false);
    const std::vector<std::string> words = split_on(params, ' ');
    for (const std::string& word : words) {
        const std::vector<std::string> parts = split_on(word, '=');
        const std::vector<std::string> summands =
            parts.size() == 2 ? split_on(parts[1], '+') : std::vector<std::string>{};
        if (summands.empty()) {
            return false;   // not an assignment; upstream skips it
        }
        for (const std::string& summand : summands) {
            const std::vector<std::string> factors = split_on(summand, '*');
            std::string factor;
            if (factors.size() == 2) {
                factor = factors[0];
            } else if (factors.size() == 1 && (factors[0] == "0" || factors[0].find('.') != std::string::npos)) {
                factor = factors[0];
            }
            if (factor.empty()) {
                return false;   // a factor of 1
            }
            double value = read_like_wcstod(factor);
            if (factor.size() > 2 && to_lower(factor.substr(factor.size() - 2)) == "db") {
                value = std::pow(10.0, value / 20.0);
            }
            // Zero as a float: at most half the smallest float, which rounds down.
            if (!(std::abs(value) <= std::numeric_limits<float>::denorm_min() / 2.0)) {
                return false;
            }
        }
        // The target as upstream resolves it, case-sensitive; a name the layout
        // does not have is a new channel, and the layout's still plays.
        const long target = channel_index(parts[0], names);
        if (target >= 0) {
            zeroed[static_cast<size_t>(target)] = true;
        }
    }
    return !names.empty() && std::find(zeroed.begin(), zeroed.end(), false) == zeroed.end();
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
    // A stream with no mask gets the default for its channel count, as upstream's
    // FilterEngine gives it before naming channels.
    const uint32_t mask = layout.speaker_mask != 0 ? layout.speaker_mask : default_speaker_mask(layout.channels);
    // Upstream walks bits 0 to 30, naming each position present in the mask in
    // order, then numbers whatever channels the mask did not cover.
    for (uint32_t i = 0; i < 31; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        if ((mask & bit) == 0) {
            continue;
        }
        const char* name = nullptr;
        for (const NamedPosition& p : kNamedPositions) {
            if (p.bit == bit) name = p.name;
        }
        names.push_back(name != nullptr ? name : std::to_string(names.size() + 1));
    }
    while (names.size() < std::min(layout.channels, kMaxApoChannels)) {
        names.push_back(std::to_string(names.size() + 1));
    }
    return names;
}

namespace {

// The name apo_channel_names gives `channel` of a layout with `mask`, without
// building it: the speaker bit when the name is a speaker's, 0 when it is the
// channel's number.
uint32_t channel_name_bit(uint32_t mask, uint32_t channel) {
    uint32_t index = 0;
    for (uint32_t i = 0; i < 31; ++i) {
        const uint32_t bit = uint32_t{1} << i;
        if ((mask & bit) == 0 || index++ != channel) {
            continue;
        }
        for (const NamedPosition& p : kNamedPositions) {
            if (p.bit == bit) return bit;
        }
        return 0;
    }
    return 0;
}

// channel_index for that name on a layout with `mask` and `channels`: a speaker
// by its position, with the SL/RL and SR/RR substitutions, or a number by its
// index. -1 when it resolves to nothing or past the channel count.
int resolve_channel(uint32_t name_bit, uint32_t channel, uint32_t mask, uint32_t channels) {
    if (name_bit == 0) {
        return channel < channels ? static_cast<int>(channel) : -1;
    }
    const auto position = [&](uint32_t bit) {
        return (mask & bit) != 0 ? std::popcount(mask & (bit - 1)) : -1;
    };
    int index = position(name_bit);
    if (index < 0) {
        if (name_bit == kSpeakerSideLeft) index = position(kSpeakerBackLeft);
        else if (name_bit == kSpeakerSideRight) index = position(kSpeakerBackRight);
        else if (name_bit == kSpeakerBackLeft) index = position(kSpeakerSideLeft);
        else if (name_bit == kSpeakerBackRight) index = position(kSpeakerSideRight);
    }
    return index >= 0 && static_cast<uint32_t>(index) < channels ? index : -1;
}

}  // namespace

void remap_channels(EqState* state, const ChannelLayout& layout) {
    const uint32_t from_channels = state->layout_channels;
    if (from_channels == 0 || layout.channels == 0) {
        return;
    }
    const uint32_t from_mask =
        state->layout_speaker_mask != 0 ? state->layout_speaker_mask : default_speaker_mask(from_channels);
    const uint32_t to_mask = layout.speaker_mask != 0 ? layout.speaker_mask : default_speaker_mask(layout.channels);
    if (from_channels == layout.channels && from_mask == to_mask) {
        return;
    }

    // Where each channel a mask can name goes, or -1.
    int to[kMaskChannels];
    for (uint32_t c = 0; c < kMaskChannels; ++c) {
        to[c] = resolve_channel(channel_name_bit(from_mask, c), c, to_mask, layout.channels);
    }
    const auto moved = [&](ChannelMask mask) {
        ChannelMask out = 0;
        for (uint32_t c = 0; c < kMaskChannels; ++c) {
            if ((mask & (ChannelMask{1} << c)) != 0 && to[c] >= 0) {
                out |= ChannelMask{1} << static_cast<uint32_t>(to[c]);
            }
        }
        return out;
    };

    for (Band& band : state->bands) {
        if (band.channels == kAllChannels) {
            continue;
        }
        const ChannelMask mask = moved(band.channels);
        if (mask != 0) {
            band.channels = mask;
        } else {
            band.enabled = false;
        }
    }

    double gain[kMaxChannels] = {};
    double delay[kMaxChannels] = {};
    SpeakerSetup& sp = state->speakers;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (to[c] >= 0 && static_cast<uint32_t>(to[c]) < kMaxChannels) {
            gain[to[c]] += state->channel_gain_db[c];
            delay[to[c]] += sp.delay_ms[c];
        }
    }
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        state->channel_gain_db[c] = gain[c];
        sp.delay_ms[c] = delay[c];
    }
    sp.inverted = moved(sp.inverted);
    sp.muted = moved(sp.muted);
    sp.small_speakers = moved(sp.small_speakers);

    state->layout_channels = layout.channels;
    state->layout_speaker_mask = layout.speaker_mask;
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
    if (layout.channels > kMaxApoChannels) {
        result.warnings.push_back({0, "a layout of " + std::to_string(layout.channels) +
                                          " channels is more than a stream can carry; read as " +
                                          std::to_string(kMaxApoChannels)});
    }
    // Channel masks, trims and the rest are indexes into this layout.
    result.state.layout_channels = std::min(layout.channels, kMaxApoChannels);
    result.state.layout_speaker_mask = layout.speaker_mask;
    ChannelMask current_mask = kAllChannels;
    bool no_channel_selected = false;
    bool stage_matches = true;
    uint32_t next_id = 1;
    size_t line_no = 0;

    std::istringstream in(text);
    std::string line;
    // Upstream catches what reading a line throws and skips that line
    // (FilterEngine::loadConfigFile); std::regex can throw error_complexity or
    // error_stack for input it finds too costly to match.
    while (std::getline(in, line)) try {
        ++line_no;

        // Upstream trims the command but not what follows the colon, only a
        // line's final CR. A Copy line's last source keeps a trailing tab.
        std::string untrimmed = line;
        if (!untrimmed.empty() && untrimmed.back() == '\r') untrimmed.pop_back();

        // A comment is a line starting with '#' (ExpressionFilterFactory); a '#'
        // later in a line is part of it, as in `Include: EQ #2.txt`.
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            result.warnings.push_back({line_no, "no ':' in line, ignored"});
            continue;
        }
        const std::string command = trim(line.substr(0, colon));
        const std::string params  = line.substr(colon + 1);

        // Upstream skips the lines under a Stage that does not match its own.
        // What is imported here plays on the render path, which both pre-mix and
        // post-mix are; a section only for capture is not.
        if (command == "Stage") {
            stage_matches = false;
            for (const std::string& part : split_spaces(to_lower(trim(params)))) {
                if (part == "pre-mix" || part == "post-mix") {
                    stage_matches = true;
                } else if (part != "capture") {
                    result.warnings.push_back({line_no, "unknown stage '" + part + "'"});
                }
            }
            if (!stage_matches) {
                result.warnings.push_back({line_no, "section not for playback, skipped"});
            }
            continue;
        }
        if (!stage_matches) {
            continue;
        }
        if (command == "Device") {
            // Which device a section is for is decided by Equalizer APO against
            // the device it runs on; here every section is imported.
            result.devices.push_back(trim(params));
            if (to_lower(trim(params)) != "all") {
                result.warnings.push_back({line_no, "Device section imported whatever device it names"});
            }
            continue;
        }
        if (command == "If" || command == "ElseIf" || command == "Else") {
            result.warnings.push_back({line_no, "condition not evaluated: every If and Else branch is imported"});
        }
        if (command == "Channel") {
            const std::vector<std::string> words = split_spaces(params);
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
        if (command == "Copy" && copy_is_mute(untrimmed.substr(untrimmed.find(':') + 1), channel_names)) {
            result.state.mute = true;
            continue;
        }
        if (command.rfind("Filter", 0) == 0) {
            const FilterPatterns& re = filter_patterns();
            const std::string norm = collapse_whitespace(normalise_decimal(params));
            if (norm.size() > kMaxFilterLineChars) {
                result.warnings.push_back({line_no, "filter line longer than " + std::to_string(kMaxFilterLineChars) +
                                                        " characters, skipped"});
                continue;
            }
            std::smatch m;
            bool enabled = true;
            if (!std::regex_search(norm, m, re.type)) {
                // "Filter 3: OFF ..." with a filter after it is one upstream skips;
                // it is read here as a disabled band, which plays the same.
                if (!std::regex_search(norm, m, re.off)) {
                    // "Filter 3: OFF" and "Filter 3: None" are legal ways to say
                    // nothing is here; upstream ignores anything else too, silently.
                    std::istringstream ps(norm);
                    std::string first;
                    ps >> first;
                    if (!first.empty() && first != "OFF" && first != "None") {
                        result.warnings.push_back({line_no, "filter line does not start with ON and a type, ignored"});
                    }
                    continue;
                }
                enabled = false;
            }
            const std::string type_token = m.str(1);
            // Everything after the type token, which is what upstream's regexes
            // are applied to.
            const std::string rest = m.suffix().str();
            if (!enabled && trim(rest).empty()) {
                continue;   // "Filter 3: OFF PK": nothing to keep
            }
            TokenInfo info{};
            if (!lookup_token(type_token, &info)) {
                if (type_token != "None") {
                    result.warnings.push_back({line_no, "unknown filter type '" + type_token + "'"});
                }
                continue;
            }
            if (no_channel_selected) {
                result.warnings.push_back(
                    {line_no, "filter ignored: the Channel line before it selects no channel"});
                continue;
            }

            Band band;
            band.id = next_id++;
            band.type = info.type;
            band.channels = current_mask;
            band.enabled = enabled;

            if (!std::regex_search(rest, m, re.freq)) {
                result.warnings.push_back({line_no, "no Fc in Hz in filter line"});
                continue;
            }
            const std::string raw = strip_nbsp(m.str(1));
            double fc = 0.0;
            if (!parse_double(raw, &fc)) {
                result.warnings.push_back({line_no, "could not read Fc"});
                continue;
            }
            band.fc = apply_rew_thousands_quirk(raw, fc);

            if (std::regex_search(rest, m, re.gain)) {
                if (type_uses_gain(info.type)) {
                    band.gain_db = number_or_zero(m.str(1));
                }
            } else if (type_uses_gain(info.type)) {
                result.warnings.push_back({line_no, "no Gain in dB for a filter type that needs one"});
                continue;
            }

            const bool is_shelf =
                info.type == FilterType::LowShelf || info.type == FilterType::HighShelf;

            // Upstream reads Q, then bandwidth for all but shelves, then a dB slope
            // written before Fc for shelves only; each one found replaces the last.
            double width = 0.0;
            WidthMode mode = WidthMode::Q;
            if (std::regex_search(rest, m, re.q)) {
                width = number_or_zero(m.str(1));
                mode = WidthMode::Q;
            }
            if (!is_shelf && std::regex_search(rest, m, re.bw)) {
                width = number_or_zero(m.str(1));
                mode = WidthMode::BandwidthOct;
            }
            if (is_shelf && std::regex_search(rest, m, re.slope)) {
                width = number_or_zero(m.str(1));
                mode = WidthMode::SlopeDb;
            }
            if (width < 0.0) {
                // Upstream designs this, unstable; the processor would play it as
                // nothing. Neither is what the file meant.
                result.warnings.push_back({line_no, "negative width, filter ignored"});
                continue;
            }
            const bool have_width = width != 0.0;
            if (have_width) {
                band.width = width;
                band.width_mode = mode;
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

        // Everything else is preserved but not modelled, and reported, once: an
        // If line already was.
        result.unsupported.push_back(line);
        if (result.warnings.empty() || result.warnings.back().line != line_no) {
            result.warnings.push_back({line_no, "'" + command + "' is not imported, line skipped"});
        }
    } catch (const std::regex_error& e) {
        result.warnings.push_back({line_no, std::string("line too costly to read, skipped: ") + e.what()});
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
    // A bandwidth shelf converts to Q; it is exact at the device's rate, or at
    // 48 kHz for an export.
    const double width_rate = options.sample_rate > 0.0 ? options.sample_rate : 48000.0;
    // LS/HS ask upstream for the corner shift, which the processor applies only
    // to Q and slope widths; a bandwidth shelf is written as LSC/HSC.
    const auto token = [&](const Band& b) {
        return token_for(b.type, b.shelf_corner && b.width_mode != WidthMode::BandwidthOct);
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

    // Non-finite values are never written: upstream would read `nan` as a
    // number. Finite ones are clamped as the processor clamps them.
    if (std::isfinite(state.preamp_db) && state.preamp_db != 0.0) {
        out << "Preamp: " << format_double(std::clamp(state.preamp_db, kMinLevelDb, kMaxLevelDb)) << " dB\n";
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

        for (const Band& given : state.bands) {
            if (given.channels != mask) {
                continue;
            }
            // As the processor plays it: width clamped, and off with no width.
            const Band b = effective_band(given);
            // A band that cannot be written as numbers is left out.
            if (!std::isfinite(b.fc) || !std::isfinite(b.gain_db) || !std::isfinite(b.width)) {
                continue;
            }
            if (!b.enabled && !options.write_disabled_as_none) {
                continue;
            }

            // A disabled band is the same line with OFF, which upstream skips and
            // the parser reads back as a disabled band.
            const bool is_shelf = b.type == FilterType::LowShelf || b.type == FilterType::HighShelf;
            out << "Filter " << index++ << ": " << (b.enabled ? "ON " : "OFF ") << token(b) << " ";
            if (b.width_mode == WidthMode::SlopeDb && is_shelf) {
                out << format_double(written_slope(b)) << " dB ";
            }
            out << "Fc " << fc_text(b) << " Hz";
            if (type_uses_gain(b.type)) {
                out << " Gain " << format_double(written_gain(b)) << " dB";
            }
            out << width_text(b, width_rate) << "\n";
        }
    }

    // Channel trims ride on Preamp inside a Channel block, which is how Peace
    // expresses them too.
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (!std::isfinite(state.channel_gain_db[c]) || state.channel_gain_db[c] == 0.0) {
            continue;
        }
        out << "Channel: " << channel_name(c) << "\n";
        out << "Preamp: " << format_double(std::clamp(state.channel_gain_db[c], kMinLevelDb, kMaxLevelDb)) << " dB\n";
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
