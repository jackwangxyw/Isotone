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

#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"

namespace isotone::compat {
namespace {

constexpr char kHeader[] =
    "# Written by Isotone: one block per audio device. Changes made here are overwritten.\n\n";
constexpr char kBypassMarker[]   = "# Isotone: bypass";
constexpr char kMuteMarker[]     = "# Isotone: mute";
constexpr char kSpeakersMarker[] = "# Isotone: speakers";
constexpr char kLayoutMarker[]   = "# Isotone: layout";
constexpr char kRoutingMarker[]  = "# Isotone: routing";
constexpr char kOutputMarker[]   = "# Isotone: output";
constexpr char kEndMarker[]      = "# Isotone: end";
constexpr char kRateGuard[]      = "If: sampleRate >= ";
constexpr char kLayoutGuard[]    = "If: outputChannelCount == ";
constexpr char kEndGuard[]       = "EndIf:";
// Upstream stores a preamp as a float gain, and 10^(-1000/20) is below the
// smallest float: exact silence on every channel, whatever the layout.
constexpr char kSilence[]        = "Preamp: -1000 dB";

// Letters only: a word starting with a digit is a channel number to upstream.
constexpr char kBassChannel[] = "ISOTONEBASS";

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// The endpoint GUID of a GUID or a full device ID ({0.0.0.00000000}.{guid}),
// which names the endpoint by its last brace group.
std::string endpoint_part(const std::string& id) {
    const size_t open = id.rfind('{');
    return open == std::string::npos ? id : id.substr(open);
}

// Braces, whitespace and case do not distinguish endpoints.
std::string guid_key(const std::string& id) {
    std::string k;
    for (char c : endpoint_part(id)) {
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
            if ((sp.muted & (ChannelMask{1} << c)) != 0) continue;   // a muted speaker sends the sub nothing
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

// Polarity, speaker mute and delay by channel name, so on any layout each acts
// on the speaker it was set for, where Equalizer APO resolves that name, and on
// nothing where the name resolves to no channel; remap_channels moves IsoAPO's
// values the same way. Values that resolve to one channel combine as
// remap_channels says: the Copy line's assignments to one target are the same,
// so the last one wins (a union); the mute selection is a union; Delay lines add.
std::string output_lines(const SpeakerSetup& sp, const ChannelLayout& layout,
                         const std::vector<std::string>& names, double sample_rate) {
    std::string out;
    // Copy resolves a target and its source against the same channels, so a name
    // this layout lacks becomes a virtual channel, never output, and the constant
    // its source is read as stays there.
    std::string copy, muted;
    for (uint32_t c = 0; c < layout.channels && c < kMaskChannels; ++c) {
        const ChannelMask bit = ChannelMask{1} << c;
        if ((sp.inverted & bit) != 0) copy += " " + names[c] + "=-1*" + names[c];
        if ((sp.muted & bit) != 0) muted += " " + names[c];
    }
    if (!copy.empty()) out += "Channel: all\nCopy:" + copy + "\n";
    if (!muted.empty()) out += "Channel:" + muted + "\n" + kSilence + "\n";

    // Lip sync on every channel, then each speaker's own delay. The processor
    // rounds their sum to whole samples once and upstream rounds each Delay line,
    // so at the device's rate each is written as a whole number of samples.
    const double max_ms = kMaxDelaySeconds * 1000.0;
    const auto samples = [&](double ms) { return std::floor(ms * sample_rate / 1000.0 + 0.5); };
    const auto longer = [&](double ms, double than_ms) {
        return sample_rate > 0.0 ? samples(ms) > samples(than_ms) : ms > than_ms;
    };
    const auto ms_text = [&](double ms, double base_ms) {
        return num(sample_rate > 0.0 ? (samples(ms) - samples(base_ms)) * 1000.0 / sample_rate : ms - base_ms);
    };
    // A channel past the per-speaker values has lip sync alone.
    const double lip_ms = channel_delay_ms(sp, kMaxChannels, max_ms);
    if (longer(lip_ms, 0.0)) {
        out += "Channel: all\nDelay: " + ms_text(lip_ms, 0.0) + " ms\n";
    }
    for (uint32_t c = 0; c < layout.channels && c < kMaxChannels; ++c) {
        const double ms = channel_delay_ms(sp, c, max_ms);
        if (longer(ms, lip_ms)) {
            out += "Channel: " + names[c] + "\nDelay: " + ms_text(ms, lip_ms) + " ms\n";
        }
    }
    return out;
}

// Channel numbers 1..channels. Upstream resolves a number within the layout's
// channel count whatever the speaker mask; a name the layout lacks resolves to
// nothing, and as a Copy source that is added as a constant (CopyFilter), which
// is DC on the output. Under the channel count guard a number always resolves.
std::vector<std::string> channel_numbers(uint32_t channels) {
    std::vector<std::string> numbers;
    for (uint32_t c = 1; c <= channels; ++c) numbers.push_back(std::to_string(c));
    return numbers;
}

// Upstream designs a frequency above Nyquist unstable. The lowest rate at which
// every band of `state` is written below the processor's clamp, so a block
// guarded by it plays at every rate where it designs what the processor does.
// 0 when there is no band to guard.
double lowest_stable_rate(const EqState& state, double sample_rate) {
    double lowest = 0.0;
    for (const Band& b : state.bands) {
        if (!b.enabled || !std::isfinite(b.fc)) continue;
        lowest = std::max(lowest, std::ceil(clamp_fc(b.fc, sample_rate) / (0.5 * kMaxFcOfNyquist)));
    }
    return lowest;
}

std::string comment_lines(const std::string& text) {
    std::string out;
    for (const std::string& line : split_lines(text)) out += "# " + line + "\n";
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
    // A stream has at most kMaxApoChannels channels; a larger count is read as that.
    device.layout = with_mask(given.layout);
    device.layout.channels = std::min(device.layout.channels, kMaxApoChannels);
    // Per-channel values written for another layout move to this one's speakers.
    remap_channels(&device.state, device.layout);
    const SpeakerSetup& sp = device.state.speakers;
    const std::vector<std::string> numbers = channel_numbers(device.layout.channels);
    const std::string layout_guard = kLayoutGuard + std::to_string(device.layout.channels) + "\n";
    std::ostringstream body;

    // Routing addresses channels by position, so it is written for one channel
    // count and guarded by it: Equalizer APO applies the file to whatever format
    // the device has when it loads, with or without Isotone running, and on
    // another count it does nothing. The speaker setup's per-speaker values are
    // for this layout, which is recorded so a reader on another can move them.
    if (!is_default(sp)) {
        body << kSpeakersMarker << " " << format_speaker_setup(sp) << "\n";
        body << kLayoutMarker << " " << device.layout.channels << " " << hex(device.layout.speaker_mask) << "\n";
    }
    const std::string routing = routing_lines(sp, device.layout, numbers);
    if (!routing.empty()) {
        body << kRoutingMarker << "\n" << layout_guard << routing << kEndGuard << "\nChannel: all\n" << kEndMarker << "\n";
    }

    // The curve: preamp and bands, which bypass turns off, then the trims,
    // which it does not. Bands are guarded by the lowest rate they are stable at;
    // the preamp is not, since the processor keeps it at every rate and routing
    // written for the channel count can sum channels into one.
    EqState bands = device.state;
    bands.mute = false;
    bands.bypass = false;
    bands.speakers = SpeakerSetup{};
    bands.preamp_db = 0.0;
    EqState preamp;
    preamp.preamp_db = device.state.preamp_db;
    EqState trims;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        trims.channel_gain_db[c] = bands.channel_gain_db[c];
        bands.channel_gain_db[c] = 0.0;
    }
    ApoFormatOptions options;
    options.layout = device.layout;
    options.sample_rate = device.sample_rate;
    std::string band_text = format_apo_config(bands, options);
    const double rate_floor = device.sample_rate > 0.0 ? lowest_stable_rate(bands, device.sample_rate) : 0.0;
    if (!band_text.empty() && rate_floor > 0.0) {
        band_text = kRateGuard + num(rate_floor) + "\n" + band_text + kEndGuard + "\n";
    }
    const std::string eq_text = format_apo_config(preamp, options) + band_text;
    if (device.state.bypass) {
        body << kBypassMarker << "\n" << comment_lines(eq_text) << kEndMarker << "\n";
    } else {
        body << eq_text;
    }
    body << format_apo_config(trims, options);

    const std::string output = output_lines(sp, device.layout, apo_channel_names(device.layout), device.sample_rate);
    if (!output.empty()) {
        body << kOutputMarker << "\n" << output << "Channel: all\n" << kEndMarker << "\n";
    }

    if (device.state.mute) {
        // Silence, as the processor's mute is, after everything else.
        body << kMuteMarker << "\nChannel: all\n" << kSilence << "\n";
    }

    return "Device: " + apo_device_pattern_for_guid(endpoint_part(device.endpoint_guid)) + "\n" + "Channel: all\n" +
           body.str();
}

std::string update_isotone_file(const std::string& existing, const DeviceConfig& device) {
    const std::string block = format_device_block(device);
    if (trim(existing).empty()) {
        return std::string(kHeader) + block;
    }
    const std::vector<Segment> segs = segments(existing);
    for (size_t i = 1; i < segs.size(); ++i) {
        if (guid_key(segs[i].device) == guid_key(device.endpoint_guid)) {
            // Upstream applies every block that matches, so a second block for
            // the device (a hand edit, another writer) is removed, not left stale.
            std::string rest;
            size_t from = trailing_blank_start(existing, segs[i]);
            for (size_t j = i + 1; j < segs.size(); ++j) {
                if (guid_key(segs[j].device) != guid_key(device.endpoint_guid)) continue;
                rest += existing.substr(from, segs[j].begin - from);
                from = segs[j].end;
            }
            rest += existing.substr(from);
            return existing.substr(0, segs[i].begin) + block + rest;
        }
    }
    std::string out = existing;
    if (out.back() != '\n') out += "\n";
    if (out.size() < 2 || out[out.size() - 2] != '\n') out += "\n";
    return out + block;
}

std::string remove_device(const std::string& existing, const std::string& endpoint_guid) {
    const std::vector<Segment> segs = segments(existing);
    std::string out;
    size_t from = 0;
    for (size_t i = 1; i < segs.size(); ++i) {
        if (guid_key(segs[i].device) != guid_key(endpoint_guid)) continue;
        out += existing.substr(from, segs[i].begin - from);
        from = segs[i].end;
    }
    return from == 0 ? existing : out + existing.substr(from);
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
        for (size_t j = 1; j < i; ++j) {
            if (guid_key(segs[j].device) == guid_key(d.endpoint_guid)) {
                d.warnings.push_back({0, "a second block for this device; Equalizer APO applies both"});
                break;
            }
        }

        // Bypass first. Its section runs to the end marker; a file written
        // before bypass left the speaker setup on has no end marker, and the
        // section runs to the end of the block.
        bool bypass = false, in_bypass = false;
        for (std::string& line : lines) {
            if (in_bypass) {
                if (line == kEndMarker) {
                    in_bypass = false;
                    line.clear();
                } else if (line.rfind("# ", 0) == 0) {
                    line = line.substr(2);
                } else if (line == "#") {
                    line.clear();
                }
            } else if (trim(line) == kBypassMarker) {
                bypass = in_bypass = true;
                line.clear();
            }
        }

        std::string curve;
        bool mute = false;
        SpeakerSetup speakers;
        ChannelLayout written{0, 0};   // 0 channels: not recorded
        const std::string speakers_prefix = std::string(kSpeakersMarker) + " ";
        const std::string layout_prefix = std::string(kLayoutMarker) + " ";
        for (size_t k = 0; k < lines.size(); ++k) {
            const std::string t = trim(lines[k]);
            if (t.rfind(speakers_prefix, 0) == 0) {
                std::string error;
                if (!parse_speaker_setup(t.substr(speakers_prefix.size()), &speakers, &error)) {
                    d.warnings.push_back({k + 2, error});
                }
                continue;
            }
            if (t.rfind(layout_prefix, 0) == 0) {
                std::istringstream in(t.substr(layout_prefix.size()));
                in >> written.channels >> std::hex >> written.speaker_mask;
                if (!in || !(in >> std::ws).eof()) {
                    written = ChannelLayout{0, 0};
                    d.warnings.push_back({k + 2, "cannot read the speaker layout '" + t + "'"});
                }
                continue;
            }
            if (t == kRoutingMarker || t == kOutputMarker) {
                // Generated from the speaker setup; skip to the end marker. Only
                // commands a section writes are skipped, so a hand edit that lost
                // the marker costs the section, not the curve after it.
                size_t end = k + 1;
                while (end < lines.size() && trim(lines[end]) != kEndMarker) {
                    const std::string s = trim(lines[end]);
                    if (!(s.rfind("Copy:", 0) == 0 || s.rfind("Channel:", 0) == 0 || s.rfind("Filter:", 0) == 0 ||
                          s.rfind("Delay:", 0) == 0 || s == kSilence || s.rfind(kLayoutGuard, 0) == 0 ||
                          s == kEndGuard)) {
                        break;
                    }
                    ++end;
                }
                if (end == lines.size() || trim(lines[end]) != kEndMarker) {
                    d.warnings.push_back({k + 2, "no end marker after " + t + "; the section ends at line " +
                                                     std::to_string(end + 2)});
                    k = end - 1;
                } else {
                    k = end;
                }
                continue;
            }
            if (t == kMuteMarker) {
                mute = true;
                // The silence that follows the marker: a -1000 dB preamp now;
                // Copy zeros, or a -100 dB preamp, in files written before.
                if (k + 2 < lines.size() && trim(lines[k + 1]) == "Channel: all" &&
                    (trim(lines[k + 2]) == kSilence || trim(lines[k + 2]).rfind("Copy:", 0) == 0 ||
                     trim(lines[k + 2]) == "Preamp: -100 dB")) {
                    k += 2;
                }
                continue;
            }
            // The rate guard around the bands.
            if (t.rfind(kRateGuard, 0) == 0 || t == kEndGuard) {
                continue;
            }
            curve += lines[k] + "\n";
        }

        const ChannelLayout layout = with_mask(layout_for(d.endpoint_guid));
        const ApoParseResult parsed = parse_apo_config(curve, layout);
        d.state = parsed.state;
        d.state.mute = mute;
        d.state.bypass = bypass;
        // The curve's channel names resolved on this layout; the speaker setup's
        // values move from the layout they were written for.
        EqState moved;
        moved.speakers = speakers;
        moved.layout_channels = written.channels;
        moved.layout_speaker_mask = written.speaker_mask;
        remap_channels(&moved, layout);
        d.state.speakers = moved.speakers;
        d.warnings.insert(d.warnings.end(), parsed.warnings.begin(), parsed.warnings.end());
        d.unsupported = parsed.unsupported;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace isotone::compat
