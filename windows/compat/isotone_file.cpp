// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

#include "isotone/processor.h"
#include "isotone/speakers.h"

namespace isotone::compat {
namespace {

constexpr char kHeader[] =
    "# Written by Isotone: one block per audio device. Changes made here are overwritten.\n\n";
constexpr char kBypassMarker[]   = "# Isotone: bypass";
constexpr char kMuteMarker[]     = "# Isotone: mute";
constexpr char kSpeakersMarker[] = "# Isotone: speakers";
constexpr char kRoutingMarker[]  = "# Isotone: routing";
constexpr char kOutputMarker[]   = "# Isotone: output";
constexpr char kEndMarker[]      = "# Isotone: end";

// Letters only: a word starting with a digit is a channel number to upstream.
constexpr char kBassChannel[] = "ISOTONEBASS";

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Braces, whitespace and case do not distinguish endpoints.
std::string guid_key(const std::string& guid) {
    std::string k;
    for (char c : guid) {
        if (c == '{' || c == '}' || std::isspace(static_cast<unsigned char>(c))) continue;
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return k;
}

// The value of a `Device:` line, or false if the line is not one.
bool device_line(const std::string& line, std::string* value) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos || trim(line.substr(0, colon)) != "Device") return false;
    *value = trim(line.substr(colon + 1));
    return true;
}

struct Segment {
    size_t begin = 0, end = 0;   // byte range in the text
    std::string device;          // empty for the header segment
};

// Splits into the header (before the first Device line) and one segment per
// Device line, each running to the next Device line. Byte ranges, so untouched
// segments can be copied exactly.
std::vector<Segment> segments(const std::string& text) {
    std::vector<Segment> out(1);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        const size_t next = nl == std::string::npos ? text.size() : nl + 1;
        std::string value;
        if (device_line(text.substr(pos, next - pos), &value)) {
            out.back().end = pos;
            Segment s;
            s.begin = pos;
            s.device = value;
            out.push_back(s);
        }
        pos = next;
    }
    out.back().end = text.size();
    return out;
}

// Trailing blank lines of a segment are the separator before the next block;
// returns where they start, which is just past the last non-blank line.
size_t trailing_blank_start(const std::string& text, const Segment& s) {
    size_t cut = s.begin;
    size_t pos = s.begin;
    while (pos < s.end) {
        const size_t nl = text.find('\n', pos);
        const size_t next = (nl == std::string::npos || nl >= s.end) ? s.end : nl + 1;
        if (!trim(text.substr(pos, next - pos)).empty()) cut = next;
        pos = next;
    }
    return cut;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// 12 significant digits and a period whatever the locale, as format_apo_config uses.
std::string num(double v) { return format_apo_number(v); }

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

bool is_default(const SpeakerSetup& s) {
    const SpeakerSetup d;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (s.delay_ms[c] != d.delay_ms[c]) return false;
    }
    return s.inverted == d.inverted && s.muted == d.muted && s.lip_sync_ms == d.lip_sync_ms &&
           s.swap_left_right == d.swap_left_right && s.swap_front_rear == d.swap_front_rear &&
           s.upmix == d.upmix && s.bass_management == d.bass_management &&
           s.crossover_hz == d.crossover_hz && s.small_speakers == d.small_speakers &&
           s.lfe_lowpass_hz == d.lfe_lowpass_hz;
}

// The same range the processor applies, with the default for a non-finite value.
double bass_hz(double hz, double fallback) {
    return std::clamp(std::isfinite(hz) ? hz : fallback, kMinBassHz, kMaxBassHz);
}

// One 24 dB/oct Linkwitz-Riley filter: two second-order Butterworth sections.
std::string lr4_lines(const char* token, double hz) {
    const std::string line = std::string("Filter: ON ") + token + " Fc " + format_apo_frequency(hz) + " Hz Q " +
                             num(std::sqrt(0.5)) + "\n";
    return line + line;
}

std::string routing_lines(const SpeakerSetup& sp, const ChannelLayout& layout,
                          const std::vector<std::string>& names) {
    std::string out;
    const uint32_t n = std::min(layout.channels, kMaxChannels);

    double m[kMaxChannels][kMaxChannels];
    routing_matrix(sp, layout.speaker_mask, layout.channels, m);
    std::string copy;
    for (uint32_t o = 0; o < n; ++o) {
        bool identity = true;
        for (uint32_t i = 0; i < n; ++i) identity &= m[o][i] == (o == i ? 1.0 : 0.0);
        if (identity) continue;
        std::string terms;
        for (uint32_t i = 0; i < n; ++i) {
            if (m[o][i] != 0.0) terms += (terms.empty() ? "" : "+") + num(m[o][i]) + "*" + names[i];
        }
        copy += " " + names[o] + "=" + (terms.empty() ? "0" : terms);
    }
    if (!copy.empty()) out += "Copy:" + copy + "\n";

    const int lfe = speaker_channel(layout.speaker_mask, layout.channels, kSpeakerLowFrequency);
    if (sp.bass_management && lfe >= 0) {
        std::string small, sum;
        for (uint32_t c = 0; c < n; ++c) {
            if (static_cast<int>(c) == lfe || (sp.small_speakers & (ChannelMask{1} << c)) == 0) continue;
            small += " " + names[c];
            sum += (sum.empty() ? "" : "+") + std::string("1*") + names[c];
        }
        const double xover = bass_hz(sp.crossover_hz, SpeakerSetup{}.crossover_hz);
        if (!sum.empty()) out += std::string("Copy: ") + kBassChannel + "=" + sum + "\n";
        out += "Channel: " + names[lfe] + "\n" +
               lr4_lines("LPQ", bass_hz(sp.lfe_lowpass_hz, SpeakerSetup{}.lfe_lowpass_hz));
        if (!sum.empty()) {
            out += std::string("Channel: ") + kBassChannel + "\n" + lr4_lines("LPQ", xover);
            out += "Channel:" + small + "\n" + lr4_lines("HPQ", xover);
            out += "Channel: all\n";
            out += "Copy: " + names[lfe] + "=1*" + names[lfe] + "+1*" + kBassChannel + "\n";
        }
    }
    return out;
}

std::string output_lines(const SpeakerSetup& sp, const ChannelLayout& layout,
                         const std::vector<std::string>& names) {
    std::string out;
    std::string copy;
    for (uint32_t c = 0; c < layout.channels && c < kMaskChannels; ++c) {
        const ChannelMask bit = ChannelMask{1} << c;
        if ((sp.muted & bit) != 0) {
            copy += " " + names[c] + "=0";
        } else if ((sp.inverted & bit) != 0) {
            copy += " " + names[c] + "=-1*" + names[c];
        }
    }
    if (!copy.empty()) out += "Copy:" + copy + "\n";

    // One Delay line per distinct delay, on the channels that share it.
    std::vector<std::pair<std::string, std::string>> groups;   // delay text, channel names
    for (uint32_t c = 0; c < layout.channels; ++c) {
        const double ms = channel_delay_ms(sp, c, kMaxDelaySeconds * 1000.0);
        if (ms <= 0.0) continue;
        const std::string key = num(ms);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == key; });
        if (it == groups.end()) {
            groups.push_back({key, names[c]});
        } else {
            it->second += " " + names[c];
        }
    }
    for (const auto& [ms, channels] : groups) {
        out += "Channel: " + channels + "\nDelay: " + ms + " ms\n";
    }
    return out;
}

}  // namespace

// A layout with no speaker mask gets the default for its channel count, as
// upstream's FilterEngine and the processor both do, so speaker positions (and
// with them swaps, upmix and bass management) are found.
static ChannelLayout with_mask(ChannelLayout layout) {
    if (layout.speaker_mask == 0) {
        layout.speaker_mask = default_speaker_mask(layout.channels);
    }
    return layout;
}

std::string format_device_block(const DeviceConfig& given) {
    DeviceConfig device = given;
    device.layout = with_mask(given.layout);
    const SpeakerSetup& sp = device.state.speakers;
    const std::vector<std::string> names = apo_channel_names(device.layout);
    std::ostringstream body;

    if (!is_default(sp)) {
        body << kSpeakersMarker << " " << format_speaker_setup(sp) << "\n";
    }
    const std::string routing = routing_lines(sp, device.layout, names);
    if (!routing.empty()) {
        body << kRoutingMarker << "\n" << routing << "Channel: all\n" << kEndMarker << "\n";
    }

    EqState curve = device.state;
    curve.mute = false;
    curve.bypass = false;
    curve.speakers = SpeakerSetup{};
    ApoFormatOptions options;
    options.layout = device.layout;
    options.sample_rate = device.sample_rate;
    body << format_apo_config(curve, options);

    const std::string output = output_lines(sp, device.layout, names);
    if (!output.empty()) {
        body << kOutputMarker << "\nChannel: all\n" << output << "Channel: all\n" << kEndMarker << "\n";
    }

    if (device.state.mute) {
        // Silence, as the processor's mute is: every output channel copied from
        // nothing, after everything else.
        std::string copy = "Copy:";
        for (uint32_t c = 0; c < device.layout.channels && c < names.size(); ++c) {
            copy += " " + names[c] + "=0";
        }
        body << kMuteMarker << "\nChannel: all\n" << copy << "\n";
    }

    std::string out = "Device: " + apo_device_pattern_for_guid(device.endpoint_guid) + "\n" +
                      "Channel: all\n";
    if (!device.state.bypass) {
        return out + body.str();
    }
    out += std::string(kBypassMarker) + "\n";
    for (const std::string& line : split_lines(body.str())) {
        out += "# " + line + "\n";
    }
    return out;
}

std::string update_isotone_file(const std::string& existing, const DeviceConfig& device) {
    const std::string block = format_device_block(device);
    if (trim(existing).empty()) {
        return std::string(kHeader) + block;
    }
    const std::vector<Segment> segs = segments(existing);
    for (size_t i = 1; i < segs.size(); ++i) {
        if (guid_key(segs[i].device) == guid_key(device.endpoint_guid)) {
            const size_t blank = trailing_blank_start(existing, segs[i]);
            return existing.substr(0, segs[i].begin) + block + existing.substr(blank);
        }
    }
    std::string out = existing;
    if (out.back() != '\n') out += "\n";
    if (out.size() < 2 || out[out.size() - 2] != '\n') out += "\n";
    return out + block;
}

std::string remove_device(const std::string& existing, const std::string& endpoint_guid) {
    const std::vector<Segment> segs = segments(existing);
    for (size_t i = 1; i < segs.size(); ++i) {
        if (guid_key(segs[i].device) == guid_key(endpoint_guid)) {
            return existing.substr(0, segs[i].begin) + existing.substr(segs[i].end);
        }
    }
    return existing;
}

std::vector<ParsedDevice> parse_isotone_file(
    const std::string& text,
    const std::function<ChannelLayout(const std::string& endpoint_guid)>& layout_for) {
    std::vector<ParsedDevice> out;
    const std::vector<Segment> segs = segments(text);
    for (size_t i = 1; i < segs.size(); ++i) {
        std::vector<std::string> lines =
            split_lines(text.substr(segs[i].begin, segs[i].end - segs[i].begin));
        lines.erase(lines.begin());   // the Device line itself

        ParsedDevice d;
        d.endpoint_guid = segs[i].device;

        // Bypass first: it comments out everything after it.
        bool bypass = false;
        for (std::string& line : lines) {
            if (bypass) {
                if (line.rfind("# ", 0) == 0) line = line.substr(2);
                else if (line == "#") line.clear();
            } else if (trim(line) == kBypassMarker) {
                bypass = true;
                line.clear();
            }
        }

        std::string curve;
        bool mute = false;
        SpeakerSetup speakers;
        const std::string speakers_prefix = std::string(kSpeakersMarker) + " ";
        for (size_t k = 0; k < lines.size(); ++k) {
            const std::string t = trim(lines[k]);
            if (t.rfind(speakers_prefix, 0) == 0) {
                std::string error;
                if (!parse_speaker_setup(t.substr(speakers_prefix.size()), &speakers, &error)) {
                    d.warnings.push_back({k + 2, error});
                }
                continue;
            }
            if (t == kRoutingMarker || t == kOutputMarker) {
                // Generated from the speaker setup; skip to the end marker.
                while (k + 1 < lines.size() && trim(lines[k + 1]) != kEndMarker) ++k;
                ++k;
                continue;
            }
            if (t == kMuteMarker) {
                mute = true;
                // The silence that follows the marker: Copy zeros now, a
                // -100 dB preamp in files written before.
                if (k + 2 < lines.size() && trim(lines[k + 1]) == "Channel: all" &&
                    (trim(lines[k + 2]).rfind("Copy:", 0) == 0 || trim(lines[k + 2]) == "Preamp: -100 dB")) {
                    k += 2;
                }
                continue;
            }
            curve += lines[k] + "\n";
        }

        const ApoParseResult parsed = parse_apo_config(curve, with_mask(layout_for(d.endpoint_guid)));
        d.state = parsed.state;
        d.state.mute = mute;
        d.state.bypass = bypass;
        d.state.speakers = speakers;
        d.warnings.insert(d.warnings.end(), parsed.warnings.begin(), parsed.warnings.end());
        d.unsupported = parsed.unsupported;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace isotone::compat
