// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-shm: the UI side of a device's shared region, from the command line.
// It is the "tiny CLI" of the stage 3 acceptance test (plan section 9): a script
// writes parameters while a measurement runs, and reads the ring back.
//
//   isotone-shm status  <endpoint>
//   isotone-shm write   <endpoint> <config.txt | -> [--bypass]
//   isotone-shm capture <endpoint> <seconds> <out.wav>
//
// <endpoint> is an endpoint GUID, with or without braces, or a full device ID
// such as {0.0.0.00000000}.{guid}. Add --local to use the Local\ namespace,
// which is where the self-test build of the APO puts its region.
//
// Output is one JSON object on stdout. Exit code 0 on success, 1 when the region
// cannot be opened or the input is unusable, 2 on bad arguments.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "isotone/apo_config.h"
#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "shared_mapping.h"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  isotone-shm status  <endpoint> [--local]\n"
                 "  isotone-shm write   <endpoint> <config.txt | -> [--bypass] [--local]\n"
                 "  isotone-shm capture <endpoint> <seconds> <out.wav> [--local]\n");
    return 2;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The last brace-wrapped group, so a full device ID and a bare GUID both work.
std::wstring endpoint_guid(const std::string& arg) {
    std::string s = arg;
    const size_t open = s.rfind('{');
    if (open != std::string::npos) {
        const size_t close = s.find('}', open);
        s = s.substr(open, close == std::string::npos ? std::string::npos : close - open + 1);
    } else {
        s = "{" + s + "}";
    }
    return std::wstring(s.begin(), s.end());
}

const char* host_state_name(uint32_t state) {
    switch (static_cast<isotone::HostState>(state)) {
        case isotone::HostState::NotLoaded: return "not_loaded";
        case isotone::HostState::Running:   return "running";
        case isotone::HostState::Error:     return "error";
    }
    return "unknown";
}

// Opens the region or prints the failure as JSON. ERROR_FILE_NOT_FOUND is the
// ordinary "no engine has created it yet" case, which the UI shows as idle.
bool open_region(isotone::win::SharedMapping* mapping, const std::wstring& name) {
    const DWORD error = mapping->open(name);
    if (error == ERROR_SUCCESS) {
        return true;
    }
    std::printf("{\"name\":%s,\"open\":false,\"error\":%lu,\"reason\":%s}\n",
                json_string(narrow(name)).c_str(), error,
                json_string(error == ERROR_FILE_NOT_FOUND ? "engine idle: no region for this endpoint"
                            : error == ERROR_INVALID_DATA ? "region has a layout this build does not understand"
                            : error == ERROR_ACCESS_DENIED ? "access denied"
                                                           : "open failed").c_str());
    return false;
}

int cmd_status(const std::wstring& name) {
    isotone::win::SharedMapping mapping;
    if (!open_region(&mapping, name)) {
        return 1;
    }
    const isotone::ParamBlock* b = mapping.params();
    const isotone::AudioRingHeader* ring = mapping.ring();

    // Liveness is the heartbeat moving, not host_state: an instance that went
    // away leaves host_state as it was. 200 ms is many process calls at any
    // realistic buffer size.
    const uint32_t beat0 = b->hdr.host_heartbeat;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint32_t beat1 = b->hdr.host_heartbeat;

    isotone::ParamBlock copy{};
    const bool consistent = isotone::param_block_read(b, &copy, 1000);

    char writer[32];
    std::snprintf(writer, sizeof(writer), "0x%016llx",
                  static_cast<unsigned long long>(ring->writer));

    std::printf("{\"name\":%s,\"open\":true,\"version\":%u,\"sample_rate\":%u,\"channels\":%u,"
                "\"host_state\":\"%s\",\"heartbeat\":%u,\"heartbeat_advancing\":%s,"
                "\"seq\":%u,\"params_consistent\":%s,\"bypass\":%s,\"mute\":%s,\"mono\":%s,"
                "\"preamp_db\":%g,\"band_count\":%u,"
                "\"ring\":{\"writer\":\"%s\",\"channels\":%u,\"capacity\":%u,\"write_index\":%u}}\n",
                json_string(narrow(name)).c_str(), b->hdr.version, b->hdr.sample_rate,
                b->hdr.channels, host_state_name(b->hdr.host_state), beat1,
                beat1 != beat0 ? "true" : "false", copy.hdr.seq, consistent ? "true" : "false",
                copy.bypass ? "true" : "false", copy.mute ? "true" : "false",
                copy.mono ? "true" : "false", static_cast<double>(copy.preamp_db),
                copy.band_count, writer, ring->channels, ring->capacity, ring->write_index);
    return 0;
}

int cmd_write(const std::wstring& name, const std::string& path, bool bypass) {
    std::string text;
    if (path == "-") {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        text = ss.str();
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::printf("{\"written\":false,\"reason\":%s}\n",
                        json_string("cannot read " + path).c_str());
            return 1;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }

    isotone::win::SharedMapping mapping;
    if (!open_region(&mapping, name)) {
        return 1;
    }

    // Channel names resolve against the device's layout. The header publishes
    // the channel count but not the speaker mask, so this uses upstream's
    // default mask for that count, which is also what upstream does for a
    // stream that reports none.
    isotone::ChannelLayout layout;
    if (mapping.params()->hdr.channels != 0) {
        layout.channels = mapping.params()->hdr.channels;
        layout.speaker_mask = isotone::default_speaker_mask(layout.channels);
    }
    isotone::ApoParseResult parsed = isotone::parse_apo_config(text, layout);
    parsed.state.bypass = bypass;

    // Say so rather than silently truncate: the block holds a fixed number.
    if (parsed.state.bands.size() > isotone::kParamMaxBands) {
        std::printf("{\"written\":false,\"reason\":%s}\n",
                    json_string("config has " + std::to_string(parsed.state.bands.size()) +
                                " bands; the param block holds " +
                                std::to_string(isotone::kParamMaxBands)).c_str());
        return 1;
    }
    isotone::param_block_write(mapping.params(), [&](isotone::ParamBlock* b) {
        isotone::to_param_block(parsed.state, b);
    });

    std::string warnings = "[";
    for (size_t i = 0; i < parsed.warnings.size(); ++i) {
        if (i) warnings += ",";
        warnings += "{\"line\":" + std::to_string(parsed.warnings[i].line) +
                    ",\"text\":" + json_string(parsed.warnings[i].text) + "}";
    }
    warnings += "]";
    std::string unsupported = "[";
    for (size_t i = 0; i < parsed.unsupported.size(); ++i) {
        if (i) unsupported += ",";
        unsupported += json_string(parsed.unsupported[i]);
    }
    unsupported += "]";

    std::printf("{\"written\":true,\"seq\":%u,\"bands\":%zu,\"preamp_db\":%g,\"bypass\":%s,"
                "\"layout\":{\"channels\":%u,\"speaker_mask\":\"0x%x\"},"
                "\"warnings\":%s,\"unsupported\":%s}\n",
                mapping.params()->hdr.seq, parsed.state.bands.size(), parsed.state.preamp_db,
                bypass ? "true" : "false", layout.channels, layout.speaker_mask, warnings.c_str(),
                unsupported.c_str());
    return 0;
}

bool write_wav(const std::string& path, const std::vector<float>& samples, uint32_t channels,
               uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    out.write("RIFF", 4);
    u32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3);   // IEEE float
    u16(static_cast<uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 4);
    u16(static_cast<uint16_t>(channels * 4));
    u16(32);
    out.write("data", 4);
    u32(data_bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return static_cast<bool>(out);
}

int cmd_capture(const std::wstring& name, double seconds, const std::string& path) {
    isotone::win::SharedMapping mapping;
    if (!open_region(&mapping, name)) {
        return 1;
    }
    const uint32_t rate = mapping.params()->hdr.sample_rate;
    if (rate == 0) {
        std::printf("{\"captured\":false,\"reason\":\"no sample rate published: the engine has not locked a format\"}\n");
        return 1;
    }

    const uint64_t wanted = static_cast<uint64_t>(seconds * rate);
    isotone::AudioRingCursor cursor;
    std::vector<float> chunk(size_t{isotone::kRingCapacityFrames} * isotone::kMaxChannels);
    std::vector<float> samples;
    uint32_t channels = 0, ch = 0;
    uint64_t frames = 0;
    uint32_t resyncs = 0;

    // Drain at about 100 Hz, which leaves the ring far from full at any rate.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(seconds * 1000) + 2000);
    isotone::audio_ring_read(mapping.ring(), &cursor, chunk.data(), isotone::kRingCapacityFrames, &ch);
    uint32_t epoch = cursor.epoch;
    while (frames < wanted && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const uint32_t n = isotone::audio_ring_read(mapping.ring(), &cursor, chunk.data(),
                                                    isotone::kRingCapacityFrames, &ch);
        if (cursor.epoch != epoch) {
            ++resyncs;   // the layout changed; what came before is a different stream
            epoch = cursor.epoch;
        }
        if (n == 0) continue;
        if (channels == 0) channels = ch;
        if (ch != channels) break;
        const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(n, wanted - frames));
        samples.insert(samples.end(), chunk.data(), chunk.data() + size_t{take} * ch);
        frames += take;
    }

    if (frames == 0 || !write_wav(path, samples, channels, rate)) {
        std::printf("{\"captured\":false,\"frames\":%llu,\"reason\":%s}\n",
                    static_cast<unsigned long long>(frames),
                    json_string(frames == 0 ? "no audio arrived: is anything playing?"
                                            : "cannot write " + path).c_str());
        return 1;
    }
    std::printf("{\"captured\":true,\"path\":%s,\"frames\":%llu,\"channels\":%u,\"sample_rate\":%u,"
                "\"complete\":%s,\"resyncs\":%u}\n",
                json_string(path).c_str(), static_cast<unsigned long long>(frames), channels, rate,
                frames == wanted ? "true" : "false", resyncs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool local = false, bypass = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--local") local = true;
        else if (a == "--bypass") bypass = true;
        else args.push_back(a);
    }
    if (args.size() < 2) {
        return usage();
    }
    const std::wstring name =
        isotone::win::mapping_name(local ? L"Local\\" : L"Global\\", endpoint_guid(args[1]));

    if (args[0] == "status" && args.size() == 2) {
        return cmd_status(name);
    }
    if (args[0] == "write" && args.size() == 3) {
        return cmd_write(name, args[2], bypass);
    }
    if (args[0] == "capture" && args.size() == 4) {
        char* end = nullptr;
        const double seconds = std::strtod(args[2].c_str(), &end);
        if (end == args[2].c_str() || !(seconds > 0.0) || seconds > 600.0) {
            return usage();
        }
        return cmd_capture(name, seconds, args[3]);
    }
    return usage();
}
