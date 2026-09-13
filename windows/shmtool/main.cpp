// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-shm: the UI side of a device's shared region, from the command line.
// It is the "tiny CLI" of the stage 3 acceptance test (plan section 9): a script
// writes parameters while a measurement runs, and reads the ring back.
//
//   isotone-shm status  <endpoint>
//   isotone-shm write   <endpoint> <config.txt | -> [--bypass] [--speakers SETTINGS]
//   isotone-shm persist <endpoint> <config.txt | -> [--bypass] [--speakers SETTINGS]
//   isotone-shm forget  <endpoint>
//   isotone-shm capture <endpoint> <seconds> <out.wav>
//
// persist saves the state a device starts with when no UI is running
// (persisted_state.h); forget deletes it, so the device starts flat.
// <endpoint> is an endpoint GUID, with or without braces, or a full device ID
// such as {0.0.0.00000000}.{guid}. SETTINGS is the speaker setup text of
// isotone/speakers.h, quoted as one argument. Add --local to use the Local\
// namespace, which is where the self-test build of the APO puts its region.
//
// Output is one JSON object on stdout. Exit code 0 on success, 1 when the region
// cannot be opened or the input is unusable, 2 on bad arguments.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include "isotone/speakers.h"
#include "persisted_state.h"
#include "shared_mapping.h"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  isotone-shm status  <endpoint> [--local]\n"
                 "  isotone-shm write   <endpoint> <config.txt | -> [--bypass] [--speakers SETTINGS] [--local]\n"
                 "  isotone-shm persist <endpoint> <config.txt | -> [--bypass] [--speakers SETTINGS] [--local]\n"
                 "  isotone-shm forget  <endpoint> [--local]\n"
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
// Empty when what is left is not a GUID.
std::wstring endpoint_guid(const std::string& arg) {
    std::string s = arg;
    const size_t open = s.rfind('{');
    if (open != std::string::npos) {
        const size_t close = s.find('}', open);
        s = s.substr(open, close == std::string::npos ? std::string::npos : close - open + 1);
    } else {
        s = "{" + s + "}";
    }
    // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    bool valid = s.size() == 38 && s.front() == '{' && s.back() == '}';
    for (size_t i = 1; valid && i < 37; ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        valid = dash ? s[i] == '-' : std::isxdigit(static_cast<unsigned char>(s[i])) != 0;
    }
    return valid ? std::wstring(s.begin(), s.end()) : std::wstring();
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

    // Shared memory is writable by any local user: a non-finite preamp is
    // reported as null, which JSON can hold.
    char preamp[32] = "null";
    if (std::isfinite(copy.preamp_db)) std::snprintf(preamp, sizeof(preamp), "%g", static_cast<double>(copy.preamp_db));
    std::printf("{\"name\":%s,\"open\":true,\"version\":%u,\"sample_rate\":%u,\"channels\":%u,"
                "\"speaker_mask\":\"0x%x\",\"host_state\":\"%s\",\"heartbeat\":%u,\"heartbeat_advancing\":%s,"
                "\"seq\":%u,\"params_consistent\":%s,\"bypass\":%s,\"mute\":%s,"
                "\"preamp_db\":%s,\"band_count\":%u,"
                "\"ring\":{\"writer\":\"%s\",\"channels\":%u,\"capacity\":%u,\"write_index\":%u}}\n",
                json_string(narrow(name)).c_str(), b->hdr.version, b->hdr.sample_rate,
                b->hdr.channels, b->hdr.speaker_mask, host_state_name(b->hdr.host_state), beat1,
                beat1 != beat0 ? "true" : "false", copy.hdr.seq, consistent ? "true" : "false",
                copy.bypass ? "true" : "false", copy.mute ? "true" : "false", preamp,
                copy.band_count, writer, ring->channels, ring->capacity, ring->write_index);
    return 0;
}

// The text of a config file, or of stdin for "-". Prints the failure as JSON
// with `verb` as the result key.
bool read_config(const std::string& path, const char* verb, std::string* text) {
    if (path == "-") {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        *text = ss.str();
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("{\"%s\":false,\"reason\":%s}\n", verb, json_string("cannot read " + path).c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *text = ss.str();
    return true;
}

// Parses for `layout` and checks the result fits a block; prints the failure
// as JSON with `verb` as the result key.
bool parse_for_block(const std::string& text, const isotone::ChannelLayout& layout, bool bypass,
                     const isotone::SpeakerSetup& speakers, const char* verb, isotone::ApoParseResult* parsed) {
    *parsed = isotone::parse_apo_config(text, layout);
    parsed->state.bypass = bypass;
    parsed->state.speakers = speakers;
    // Say so rather than silently truncate: the block holds a fixed number.
    if (parsed->state.bands.size() > isotone::kParamMaxBands) {
        std::printf("{\"%s\":false,\"reason\":%s}\n", verb,
                    json_string("config has " + std::to_string(parsed->state.bands.size()) +
                                " bands; the param block holds " + std::to_string(isotone::kParamMaxBands))
                        .c_str());
        return false;
    }
    return true;
}

std::string warnings_json(const isotone::ApoParseResult& parsed) {
    std::string warnings = "[";
    for (size_t i = 0; i < parsed.warnings.size(); ++i) {
        if (i) warnings += ",";
        warnings += "{\"line\":" + std::to_string(parsed.warnings[i].line) +
                    ",\"text\":" + json_string(parsed.warnings[i].text) + "}";
    }
    return warnings + "]";
}

std::string unsupported_json(const isotone::ApoParseResult& parsed) {
    std::string unsupported = "[";
    for (size_t i = 0; i < parsed.unsupported.size(); ++i) {
        if (i) unsupported += ",";
        unsupported += json_string(parsed.unsupported[i]);
    }
    return unsupported + "]";
}

int cmd_write(const std::wstring& name, const std::string& path, bool bypass,
              const isotone::SpeakerSetup& speakers) {
    std::string text;
    if (!read_config(path, "written", &text)) {
        return 1;
    }

    isotone::win::SharedMapping mapping;
    if (!open_region(&mapping, name)) {
        return 1;
    }

    // Channel names resolve against the device's layout, which the host
    // publishes in the header.
    isotone::ChannelLayout layout;
    if (mapping.params()->hdr.channels != 0) {
        layout.channels = mapping.params()->hdr.channels;
        layout.speaker_mask = mapping.params()->hdr.speaker_mask;
    }
    isotone::ApoParseResult parsed;
    if (!parse_for_block(text, layout, bypass, speakers, "written", &parsed)) {
        return 1;
    }
    isotone::param_block_write(mapping.params(), [&](isotone::ParamBlock* b) {
        isotone::to_param_block(parsed.state, b);
    });

    const std::string warnings = warnings_json(parsed);
    const std::string unsupported = unsupported_json(parsed);

    std::printf("{\"written\":true,\"seq\":%u,\"bands\":%zu,\"preamp_db\":%g,\"bypass\":%s,"
                "\"layout\":{\"channels\":%u,\"speaker_mask\":\"0x%x\"},\"speakers\":%s,"
                "\"warnings\":%s,\"unsupported\":%s}\n",
                mapping.params()->hdr.seq, parsed.state.bands.size(), parsed.state.preamp_db,
                bypass ? "true" : "false", layout.channels, layout.speaker_mask,
                json_string(isotone::format_speaker_setup(speakers)).c_str(), warnings.c_str(),
                unsupported.c_str());
    return 0;
}

int cmd_persist(const std::wstring& name, const std::wstring& file, const std::string& path, bool bypass,
                const isotone::SpeakerSetup& speakers) {
    std::string text;
    if (!read_config(path, "persisted", &text)) {
        return 1;
    }
    // The device's layout if an engine has published it; 7.1 otherwise, and
    // the output says which.
    isotone::ChannelLayout layout;
    isotone::win::SharedMapping mapping;
    const bool published = mapping.open(name) == ERROR_SUCCESS && mapping.params()->hdr.channels != 0;
    if (published) {
        layout.channels = mapping.params()->hdr.channels;
        layout.speaker_mask = mapping.params()->hdr.speaker_mask;
    }
    isotone::ApoParseResult parsed;
    if (!parse_for_block(text, layout, bypass, speakers, "persisted", &parsed)) {
        return 1;
    }
    isotone::ParamBlock block{};
    isotone::init_param_block(&block);
    isotone::to_param_block(parsed.state, &block);
    const DWORD error = isotone::win::write_persisted_state(file, block);
    if (error != ERROR_SUCCESS) {
        std::printf("{\"persisted\":false,\"path\":%s,\"error\":%lu}\n", json_string(narrow(file)).c_str(), error);
        return 1;
    }
    std::printf("{\"persisted\":true,\"path\":%s,\"bands\":%zu,\"preamp_db\":%g,\"bypass\":%s,"
                "\"layout\":{\"channels\":%u,\"speaker_mask\":\"0x%x\",\"from_engine\":%s},\"speakers\":%s,"
                "\"warnings\":%s,\"unsupported\":%s}\n",
                json_string(narrow(file)).c_str(), parsed.state.bands.size(), parsed.state.preamp_db,
                bypass ? "true" : "false", layout.channels, layout.speaker_mask, published ? "true" : "false",
                json_string(isotone::format_speaker_setup(speakers)).c_str(), warnings_json(parsed).c_str(),
                unsupported_json(parsed).c_str());
    return 0;
}

int cmd_forget(const std::wstring& file) {
    const bool deleted = DeleteFileW(file.c_str()) != 0;
    const DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
    std::printf("{\"forgotten\":%s,\"path\":%s,\"error\":%lu}\n",
                deleted || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "true" : "false",
                json_string(narrow(file)).c_str(), error);
    return deleted || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : 1;
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
    // A WAV file's sizes are 32-bit; refuse rather than write a corrupt header.
    if (wanted * isotone::kMaxChannels * sizeof(float) > 0xFFFFFFFFull - 44) {
        std::printf("{\"captured\":false,\"reason\":%s}\n",
                    json_string("that capture would exceed the 4 GB a WAV file can hold; capture less").c_str());
        return 2;
    }
    isotone::AudioRingCursor cursor;
    std::vector<float> chunk(size_t{isotone::kRingCapacityFrames} * isotone::kMaxChannels);
    std::vector<float> samples;
    uint32_t channels = 0, ch = 0;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    uint32_t resyncs = 0;

    // Drain at about 100 Hz, which leaves the ring far from full at any rate.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(seconds * 1000) + 2000);
    isotone::audio_ring_read(mapping.ring(), isotone::kRingCapacityFrames, &cursor, chunk.data(), isotone::kRingCapacityFrames, &ch);
    uint32_t epoch = cursor.epoch;
    while (frames < wanted && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const isotone::AudioRingCursor before = cursor;
        const uint32_t n = isotone::audio_ring_read(mapping.ring(), isotone::kRingCapacityFrames, &cursor, chunk.data(),
                                                    isotone::kRingCapacityFrames, &ch);
        // Frames the ring moved past that the read did not return were lost.
        if (cursor.epoch == before.epoch) dropped += (cursor.next - before.next) - n;
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
    // Complete means every frame asked for, from one stream, contiguous.
    std::printf("{\"captured\":true,\"path\":%s,\"frames\":%llu,\"channels\":%u,\"sample_rate\":%u,"
                "\"complete\":%s,\"resyncs\":%u,\"dropped_frames\":%llu}\n",
                json_string(path).c_str(), static_cast<unsigned long long>(frames), channels, rate,
                frames == wanted && resyncs == 0 && dropped == 0 ? "true" : "false", resyncs,
                static_cast<unsigned long long>(dropped));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool local = false, bypass = false;
    isotone::SpeakerSetup speakers;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--local") local = true;
        else if (a == "--bypass") bypass = true;
        else if (a == "--speakers") {
            std::string error;
            if (i + 1 == argc || !isotone::parse_speaker_setup(argv[++i], &speakers, &error)) {
                if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
                return usage();
            }
        }
        else args.push_back(a);
    }
    if (args.size() < 2) {
        return usage();
    }
    const std::wstring guid = endpoint_guid(args[1]);
    if (guid.empty()) {
        return usage();
    }
    const std::wstring name = isotone::win::mapping_name(local ? L"Local\\" : L"Global\\", guid);

    if (args[0] == "status" && args.size() == 2) {
        return cmd_status(name);
    }
    if (args[0] == "write" && args.size() == 3) {
        return cmd_write(name, args[2], bypass, speakers);
    }
    const std::wstring saved = isotone::win::persisted_state_path(isotone::win::persisted_state_dir(local), guid);
    if (args[0] == "persist" && args.size() == 3) {
        return cmd_persist(name, saved, args[2], bypass, speakers);
    }
    if (args[0] == "forget" && args.size() == 2) {
        return cmd_forget(saved);
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
