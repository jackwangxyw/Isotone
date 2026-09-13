// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/speakers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace isotone {

namespace {

// 12 significant digits, as format_apo_config uses.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return buf;
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

}  // namespace

void routing_matrix(const SpeakerSetup& setup, uint32_t speaker_mask, uint32_t channels,
                    double m[kMaxChannels][kMaxChannels]) {
    const int n = static_cast<int>(std::min(channels, kMaxChannels));
    const int fl = speaker_channel(speaker_mask, channels, kSpeakerFrontLeft);
    const int fr = speaker_channel(speaker_mask, channels, kSpeakerFrontRight);
    const int fc = speaker_channel(speaker_mask, channels, kSpeakerFrontCenter);
    const int bl = speaker_channel(speaker_mask, channels, kSpeakerBackLeft);
    const int br = speaker_channel(speaker_mask, channels, kSpeakerBackRight);
    const int sl = speaker_channel(speaker_mask, channels, kSpeakerSideLeft);
    const int sr = speaker_channel(speaker_mask, channels, kSpeakerSideRight);

    // Upmix first: the fronts feed the other speakers.
    double up[kMaxChannels][kMaxChannels];
    for (int o = 0; o < static_cast<int>(kMaxChannels); ++o) {
        for (int i = 0; i < static_cast<int>(kMaxChannels); ++i) {
            up[o][i] = o == i ? 1.0 : 0.0;
        }
    }
    auto feed = [&](int to, int from, double gain) {
        if (to >= 0 && from >= 0 && to < n && from < n) up[to][from] += gain;
    };
    if (setup.upmix != Upmix::Off) {
        const double g = std::sqrt(0.5);
        if (setup.upmix == Upmix::All) {
            feed(fc, fl, 0.5);
            feed(fc, fr, 0.5);
        }
        feed(sl, fl, g);
        feed(sr, fr, g);
        feed(bl, fl, g);
        feed(br, fr, g);
    }

    // Then the swaps pick which of those signals each speaker plays. Both are
    // permutations of the same four speakers and commute.
    int source[kMaxChannels];
    for (int o = 0; o < static_cast<int>(kMaxChannels); ++o) source[o] = o;
    auto swap = [&](int a, int b) {
        if (a >= 0 && b >= 0 && a < n && b < n) std::swap(source[a], source[b]);
    };
    if (setup.swap_front_rear) {
        swap(fl, bl);
        swap(fr, br);
    }
    if (setup.swap_left_right) {
        swap(fl, fr);
        swap(bl, br);
        swap(sl, sr);
    }
    for (int o = 0; o < static_cast<int>(kMaxChannels); ++o) {
        for (int i = 0; i < static_cast<int>(kMaxChannels); ++i) {
            m[o][i] = up[source[o]][i];
        }
    }
}

double channel_delay_ms(const SpeakerSetup& setup, uint32_t channel, double max_ms) {
    auto clean = [&](double v) { return std::isfinite(v) ? std::clamp(v, 0.0, max_ms) : 0.0; };
    const double own = channel < kMaxChannels ? clean(setup.delay_ms[channel]) : 0.0;
    return std::min(clean(setup.lip_sync_ms) + own, max_ms);
}

std::string format_speaker_setup(const SpeakerSetup& s) {
    std::string delays;
    for (uint32_t c = 0; c < kMaxChannels; ++c) delays += (c ? "," : "") + num(s.delay_ms[c]);
    const char* upmix = s.upmix == Upmix::All ? "all" : s.upmix == Upmix::NoCentre ? "nocentre" : "off";
    return "delay_ms=" + delays + " lip_sync_ms=" + num(s.lip_sync_ms) + " inverted=" + hex(s.inverted) +
           " muted=" + hex(s.muted) + " swap_left_right=" + (s.swap_left_right ? "1" : "0") +
           " swap_front_rear=" + (s.swap_front_rear ? "1" : "0") + " upmix=" + upmix +
           " bass_management=" + (s.bass_management ? "1" : "0") + " crossover_hz=" + num(s.crossover_hz) +
           " small_speakers=" + hex(s.small_speakers) + " lfe_lowpass_hz=" + num(s.lfe_lowpass_hz);
}

bool parse_speaker_setup(const std::string& text, SpeakerSetup* setup, std::string* error) {
    SpeakerSetup s;
    std::istringstream in(text);
    std::string token;
    const auto number = [](const std::string& v, double* out) {
        char* end = nullptr;
        *out = std::strtod(v.c_str(), &end);
        return !v.empty() && end == v.c_str() + v.size() && std::isfinite(*out);
    };
    const auto mask = [](const std::string& v, uint32_t* out) {
        char* end = nullptr;
        const unsigned long long x = std::strtoull(v.c_str(), &end, 0);
        *out = static_cast<uint32_t>(x);
        return !v.empty() && end == v.c_str() + v.size() && x <= 0xFFFFFFFFull;
    };
    const auto flag = [](const std::string& v, bool* out) {
        if (v != "0" && v != "1") return false;
        *out = v == "1";
        return true;
    };
    while (in >> token) {
        const size_t eq = token.find('=');
        const std::string key = token.substr(0, eq);
        const std::string value = eq == std::string::npos ? std::string() : token.substr(eq + 1);
        bool ok = eq != std::string::npos;
        if (!ok) {
        } else if (key == "delay_ms") {
            std::istringstream parts(value);
            std::string part;
            uint32_t c = 0;
            while (ok && std::getline(parts, part, ',')) {
                ok = c < kMaxChannels && number(part, &s.delay_ms[c++]);
            }
        } else if (key == "lip_sync_ms") {
            ok = number(value, &s.lip_sync_ms);
        } else if (key == "inverted") {
            ok = mask(value, &s.inverted);
        } else if (key == "muted") {
            ok = mask(value, &s.muted);
        } else if (key == "swap_left_right") {
            ok = flag(value, &s.swap_left_right);
        } else if (key == "swap_front_rear") {
            ok = flag(value, &s.swap_front_rear);
        } else if (key == "upmix") {
            ok = value == "off" || value == "all" || value == "nocentre";
            s.upmix = value == "all" ? Upmix::All : value == "nocentre" ? Upmix::NoCentre : Upmix::Off;
        } else if (key == "bass_management") {
            ok = flag(value, &s.bass_management);
        } else if (key == "crossover_hz") {
            ok = number(value, &s.crossover_hz);
        } else if (key == "small_speakers") {
            ok = mask(value, &s.small_speakers);
        } else if (key == "lfe_lowpass_hz") {
            ok = number(value, &s.lfe_lowpass_hz);
        } else {
            ok = false;
        }
        if (!ok) {
            *error = "cannot read speaker setting '" + token + "'";
            return false;
        }
    }
    *setup = s;
    return true;
}

}  // namespace isotone
