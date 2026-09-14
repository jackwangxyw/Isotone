// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-compat: the Equalizer APO compatibility backend from the command line.
//
//   isotone-compat locate
//   isotone-compat inspect  [root]
//   isotone-compat attach   [root]
//   isotone-compat detach   [root]
//   isotone-compat apply    [root] --device <guid> --channels N [--mask 0xMASK] --rate HZ
//                           [--bypass] [--mute] [--speakers "key=value ..."] <config.txt | ->
//
// apply needs the device's format: the block is written for that channel count
// and rate, and does nothing on another.
//   isotone-compat show     [root] [--channels N --mask 0xMASK]
//   isotone-compat loopback --render <guid> --seconds <s> <out.wav>
//
// [root] is where config.txt and Isotone.txt live:
//   (default)          .\sim\compat-sandbox, created with an empty config.txt
//   --root <dir>       any directory except the live install's config directory
//   --real-install     the directory the installed Equalizer APO reads
//
// The live directory is reachable only through --real-install, so a mistyped
// --root cannot write to it. Output is one JSON object; exit 0 on success, 1 on
// failure, 2 on bad arguments.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "compat_writer.h"
#include "config_files.h"
#include "eapo_install.h"
#include "isotone/param_block.h"
#include "isotone/speakers.h"
#include "isotone_file.h"
#include "loopback_capture.h"

namespace fs = std::filesystem;
using namespace isotone;
using namespace isotone::compat;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  isotone-compat locate\n"
                 "  isotone-compat inspect  [--root DIR | --real-install]\n"
                 "  isotone-compat attach   [--root DIR | --real-install]\n"
                 "  isotone-compat detach   [--root DIR | --real-install]\n"
                 "  isotone-compat apply    [--root DIR | --real-install] --device GUID\n"
                 "                          --channels N [--mask 0xMASK] --rate HZ [--bypass] [--mute] [--speakers SETTINGS]\n"
                 "                          <config.txt | ->\n"
                 "  isotone-compat show     [--root DIR | --real-install] [--channels N --mask 0xMASK]\n"
                 "  isotone-compat loopback --render GUID --seconds S <out.wav>\n");
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

std::string utf8(const fs::path& p) {
    const std::wstring w = p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Arguments are held as UTF-8 (wmain converts them); a path is made from one
// through UTF-16, never through the ANSI code page.
fs::path path_from_utf8(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string error_text(DWORD e) {
    wchar_t buf[256] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, 256, nullptr);
    std::string s = utf8(fs::path(buf));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

int fail(const std::string& what, DWORD e) {
    std::printf("{\"ok\":false,\"error\":%lu,\"reason\":%s}\n", e,
                json_string(what + (e ? ": " + error_text(e) : "")).c_str());
    return 1;
}

struct Args {
    std::vector<std::string> positional;
    std::string root, device, mask, render, speakers;
    uint32_t channels = 0;
    double seconds = 0.0;
    double rate = 0.0;
    bool real_install = false, bypass = false, mute = false, bad = false;
};

Args parse_args(int argc, const std::vector<std::string>& argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                a.bad = true;
                return {};
            }
            return argv[++i];
        };
        if (s == "--root") a.root = value();
        else if (s == "--real-install") a.real_install = true;
        else if (s == "--device") a.device = value();
        else if (s == "--channels") a.channels = static_cast<uint32_t>(std::strtoul(value().c_str(), nullptr, 10));
        else if (s == "--mask") a.mask = value();
        else if (s == "--render") a.render = value();
        else if (s == "--seconds") a.seconds = std::strtod(value().c_str(), nullptr);
        else if (s == "--rate") a.rate = std::strtod(value().c_str(), nullptr);
        else if (s == "--bypass") a.bypass = true;
        else if (s == "--mute") a.mute = true;
        else if (s == "--speakers") a.speakers = value();
        else if (s.rfind("--", 0) == 0 && s != "-") a.bad = true;
        else a.positional.push_back(s);
    }
    return a;
}

// Resolves the config directory, enforcing that the live install is reached
// only on purpose. Returns false after printing the failure.
bool resolve_root(const Args& a, fs::path* root, int* rc) {
    if (a.real_install) {
        if (!a.root.empty()) {
            *rc = usage();
            return false;
        }
        const EqualizerApoInstall install = locate_equalizer_apo();
        if (install.error != ERROR_SUCCESS) {
            *rc = fail("Equalizer APO is not installed", install.error);
            return false;
        }
        *root = install.config_path;
        return true;
    }
    if (!a.root.empty()) {
        if (is_live_install_path(path_from_utf8(a.root))) {
            std::printf("{\"ok\":false,\"reason\":%s}\n",
                        json_string("that is inside the live Equalizer APO install; "
                                    "pass --real-install to mean it").c_str());
            *rc = 1;
            return false;
        }
        *root = path_from_utf8(a.root);
        return true;
    }
    // The default sandbox is created on demand, with an empty config.txt. The
    // sandbox path itself is checked, before anything is created, since `sim`
    // or the sandbox could be a junction into the live install.
    *root = fs::current_path() / "sim" / "compat-sandbox";
    if (is_live_install_path(fs::current_path()) || is_live_install_path(*root)) {
        std::printf("{\"ok\":false,\"reason\":%s}\n",
                    json_string("the current directory is inside the live Equalizer APO install; "
                                "run from elsewhere or pass --root").c_str());
        *rc = 1;
        return false;
    }
    std::error_code ec;
    fs::create_directories(*root, ec);
    if (ec) {
        *rc = fail("cannot create " + utf8(*root), static_cast<DWORD>(ec.value()));
        return false;
    }
    if (is_live_install_path(*root)) {
        *rc = fail("the sandbox resolves into the live Equalizer APO install", ERROR_ACCESS_DENIED);
        return false;
    }
    if (!fs::exists(*root / "config.txt")) {
        std::ofstream(*root / "config.txt", std::ios::binary);
    }
    return true;
}

ChannelLayout layout_from(const Args& a) {
    ChannelLayout layout;
    if (a.channels != 0) {
        layout.channels = a.channels;
        layout.speaker_mask = a.mask.empty()
                                  ? default_speaker_mask(a.channels)
                                  : static_cast<uint32_t>(std::strtoul(a.mask.c_str(), nullptr, 16));
    }
    return layout;
}

std::string inspection_json(const ConfigInspection& i) {
    std::string includes = "[";
    for (size_t k = 0; k < i.includes.size(); ++k) {
        includes += (k ? "," : "") + json_string(i.includes[k]);
    }
    includes += "]";
    return std::string("{\"isotone_included\":") + (i.isotone_included ? "true" : "false") +
           ",\"isotone_included_conditionally\":" + (i.isotone_included_conditionally ? "true" : "false") +
           ",\"attached_by_isotone\":" + (i.attached_by_isotone ? "true" : "false") +
           ",\"peace_included\":" + (i.peace_included ? "true" : "false") +
           ",\"has_stage_lines\":" + (i.has_stage_lines ? "true" : "false") +
           ",\"has_conditionals\":" + (i.has_conditionals ? "true" : "false") +
           ",\"open_ifs\":" + std::to_string(i.open_ifs) +
           ",\"stage_changed_at_end\":" + (i.stage_changed_at_end ? "true" : "false") +
           ",\"includes\":" + includes + "}";
}

// Written atomically, so a name planted as a link cannot redirect the write.
DWORD write_wav(const fs::path& path, const std::vector<float>& samples, uint32_t channels, uint32_t rate) {
    const uint64_t data_bytes64 = uint64_t{samples.size()} * sizeof(float);
    if (data_bytes64 > 0xFFFFFFFFull - 36) return ERROR_FILE_TOO_LARGE;
    const uint32_t data_bytes = static_cast<uint32_t>(data_bytes64);
    std::string out;
    out.reserve(44 + data_bytes);
    const auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
    out += "RIFF";
    u32(36 + data_bytes);
    out += "WAVEfmt ";
    u32(16);
    u16(3);
    u16(static_cast<uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 4);
    u16(static_cast<uint16_t>(channels * 4));
    u16(32);
    out += "data";
    u32(data_bytes);
    out.append(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return write_file_atomically(path, out);
}

int cmd_loopback(const Args& a) {
    if (a.render.empty() || !(a.seconds > 0.0) || a.seconds > 600.0 || a.positional.size() != 2) {
        return usage();
    }
    const fs::path out_path = path_from_utf8(a.positional[1]);
    std::error_code ec;
    if (is_live_install_path(fs::absolute(out_path, ec).parent_path())) {
        std::printf("{\"ok\":false,\"reason\":%s}\n",
                    json_string("the output path is inside the live Equalizer APO install").c_str());
        return 1;
    }
    LoopbackCapture capture;
    const HRESULT hr = capture.start(a.render);
    if (FAILED(hr)) return fail("loopback start failed", static_cast<DWORD>(hr));

    const uint64_t wanted = static_cast<uint64_t>(a.seconds * capture.sample_rate());
    std::vector<float> chunk(size_t{kRingCapacityFrames} * kMaxChannels), samples;
    AudioRingCursor cursor;
    uint32_t ch = 0, channels = 0;
    uint64_t frames = 0;
    // Synchronise first, so the capture starts now rather than with stale frames.
    capture.read(&cursor, chunk.data(), kRingCapacityFrames, &ch);
    // A glitch before this point is not in the capture.
    const uint32_t glitches_before = capture.discontinuities();
    uint64_t dropped = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(a.seconds * 1000) + 3000);
    while (frames < wanted && std::chrono::steady_clock::now() < deadline && capture.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const AudioRingCursor before = cursor;
        const uint32_t n = capture.read(&cursor, chunk.data(), kRingCapacityFrames, &ch);
        // Frames the ring moved past that the read did not return were lost.
        if (cursor.epoch == before.epoch) dropped += (cursor.next - before.next) - n;
        if (n == 0) continue;
        if (channels == 0) channels = ch;
        if (ch != channels) break;
        const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(n, wanted - frames));
        samples.insert(samples.end(), chunk.data(), chunk.data() + size_t{take} * ch);
        frames += take;
    }
    const HRESULT thread_hr = capture.thread_error();
    const uint32_t rate = capture.sample_rate();
    const uint32_t glitches = capture.discontinuities() - glitches_before;
    capture.stop();
    if (frames == 0) {
        return fail(FAILED(thread_hr) ? "loopback stream failed" : "no audio arrived: loopback delivers nothing while the endpoint is idle",
                    static_cast<DWORD>(thread_hr));
    }
    if (const DWORD e = write_wav(out_path, samples, channels, rate); e != ERROR_SUCCESS) {
        return fail("cannot write " + a.positional[1], e);
    }
    // Complete means every frame asked for, contiguous: a phase or level read
    // across a glitch is not a measurement.
    std::printf("{\"ok\":true,\"path\":%s,\"frames\":%llu,\"channels\":%u,\"stream_channels\":%u,"
                "\"sample_rate\":%u,\"discontinuities\":%u,\"dropped_frames\":%llu,\"complete\":%s}\n",
                json_string(a.positional[1]).c_str(), static_cast<unsigned long long>(frames), channels,
                capture.channels(), rate, glitches, static_cast<unsigned long long>(dropped),
                frames == wanted && glitches == 0 && dropped == 0 ? "true" : "false");
    return 0;
}

}  // namespace

// wmain: arguments arrive as UTF-16 and are turned into UTF-8, so a path outside
// the ANSI code page survives and the JSON output stays UTF-8.
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) argv.push_back(utf8(fs::path(wargv[i])));
    const Args a = parse_args(argc, argv);
    if (a.bad || a.positional.empty()) return usage();
    const std::string& cmd = a.positional[0];

    if (cmd == "locate") {
        if (a.positional.size() != 1) return usage();
        const EqualizerApoInstall i = locate_equalizer_apo();
        if (i.error != ERROR_SUCCESS) return fail("Equalizer APO not found", static_cast<DWORD>(i.error));
        std::printf("{\"ok\":true,\"install_path\":%s,\"config_path\":%s}\n",
                    json_string(utf8(i.install_path)).c_str(), json_string(utf8(i.config_path)).c_str());
        return 0;
    }
    if (cmd == "loopback") {
        return cmd_loopback(a);   // the capture thread initialises COM itself
    }

    fs::path root;
    int rc = 0;
    if (cmd != "inspect" && cmd != "attach" && cmd != "detach" && cmd != "apply" && cmd != "show") {
        return usage();
    }
    if (!resolve_root(a, &root, &rc)) return rc;

    if (cmd == "inspect" && a.positional.size() == 1) {
        const ConfigInspection i = inspect_config(root);
        if (i.error != ERROR_SUCCESS) return fail("cannot read config.txt in " + utf8(root), i.error);
        std::printf("{\"ok\":true,\"root\":%s,\"config\":%s}\n", json_string(utf8(root)).c_str(),
                    inspection_json(i).c_str());
        return 0;
    }
    if (cmd == "attach" && a.positional.size() == 1) {
        const AttachResult r = attach_include(root);
        if (r.error == ERROR_ALREADY_EXISTS) {
            return fail("Isotone.txt is already included under a Device, If or Stage line, where only some devices "
                        "or Equalizer APO instances reach it; "
                        "remove that line by hand",
                        r.error);
        }
        if (r.error != ERROR_SUCCESS) return fail("attach failed in " + utf8(root), r.error);
        std::printf("{\"ok\":true,\"root\":%s,\"appended\":%s,\"backup\":%s,\"before\":%s}\n",
                    json_string(utf8(root)).c_str(), r.appended ? "true" : "false",
                    json_string(utf8(r.backup)).c_str(), inspection_json(r.before).c_str());
        return 0;
    }
    if (cmd == "detach" && a.positional.size() == 1) {
        bool removed = false;
        const DWORD e = detach_include(root, &removed);
        if (e != ERROR_SUCCESS) {
            return fail(e == ERROR_INVALID_DATA
                            ? "Isotone.txt is included, but not by the block attach wrote; remove it by hand"
                            : "detach failed",
                        e);
        }
        std::printf("{\"ok\":true,\"root\":%s,\"removed\":%s}\n", json_string(utf8(root)).c_str(),
                    removed ? "true" : "false");
        return 0;
    }
    if (cmd == "apply" && a.positional.size() == 2 && !a.device.empty() && a.channels != 0 && a.rate > 0.0) {
        std::string text;
        if (a.positional[1] == "-") {
            std::ostringstream ss;
            ss << std::cin.rdbuf();
            text = ss.str();
        } else if (const DWORD e = read_file_bytes(path_from_utf8(a.positional[1]), &text); e != ERROR_SUCCESS) {
            return fail("cannot read " + a.positional[1], e);
        }
        DeviceConfig device;
        // A full device ID ({0.0.0.00000000}.{guid}) names the endpoint by its
        // last brace group; the Device line needs the GUID alone.
        const size_t open = a.device.rfind('{');
        device.endpoint_guid = open == std::string::npos ? a.device : a.device.substr(open);
        device.layout = layout_from(a);
        device.sample_rate = a.rate;
        ApoParseResult parsed = parse_apo_config(text, device.layout);
        device.state = parsed.state;
        device.state.bypass = a.bypass;
        device.state.mute = a.mute;
        if (std::string error; !parse_speaker_setup(a.speakers, &device.state.speakers, &error)) {
            return fail(error, ERROR_INVALID_PARAMETER);
        }

        CompatWriter writer(root);
        if (const DWORD e = writer.load(); e != ERROR_SUCCESS) return fail("cannot load Isotone.txt", e);
        if (const DWORD e = writer.persist(device); e != ERROR_SUCCESS) {
            return fail("cannot write " + utf8(writer.path()), e);
        }
        std::string warnings = "[";
        for (size_t k = 0; k < parsed.warnings.size(); ++k) {
            warnings += (k ? "," : "") + std::string("{\"line\":") + std::to_string(parsed.warnings[k].line) +
                        ",\"text\":" + json_string(parsed.warnings[k].text) + "}";
        }
        warnings += "]";
        // Lines the backend does not model (Copy, Delay, GraphicEQ, Include, ...)
        // and Device sections in the input are not written; say so.
        std::string unsupported = "[";
        for (size_t k = 0; k < parsed.unsupported.size(); ++k) {
            unsupported += (k ? "," : "") + json_string(parsed.unsupported[k]);
        }
        unsupported += "]";
        std::string devices = "[";
        for (size_t k = 0; k < parsed.devices.size(); ++k) {
            devices += (k ? "," : "") + json_string(parsed.devices[k]);
        }
        devices += "]";
        std::printf("{\"ok\":true,\"path\":%s,\"bands\":%zu,\"warnings\":%s,\"not_applied\":%s,"
                    "\"ignored_device_lines\":%s,\"attached\":%s}\n",
                    json_string(utf8(writer.path())).c_str(), device.state.bands.size(),
                    warnings.c_str(), unsupported.c_str(), devices.c_str(),
                    inspect_config(root).isotone_included ? "true" : "false");
        return 0;
    }
    if (cmd == "show" && a.positional.size() == 1) {
        std::string text;
        const DWORD e = read_file_bytes(root / kIsotoneFileName, &text);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return fail("cannot read Isotone.txt", e);
        const ChannelLayout layout = layout_from(a);
        const auto devices = parse_isotone_file(text, [&](const std::string&) { return layout; });
        std::string list = "[";
        for (size_t k = 0; k < devices.size(); ++k) {
            const ParsedDevice& d = devices[k];
            list += (k ? "," : "") + std::string("{\"device\":") + json_string(d.endpoint_guid) +
                    ",\"bands\":" + std::to_string(d.state.bands.size()) +
                    ",\"preamp_db\":" + std::to_string(d.state.preamp_db) +
                    ",\"bypass\":" + (d.state.bypass ? "true" : "false") +
                    ",\"mute\":" + (d.state.mute ? "true" : "false") +
                    ",\"speakers\":" + json_string(format_speaker_setup(d.state.speakers)) +
                    ",\"warnings\":" + std::to_string(d.warnings.size()) + "}";
        }
        list += "]";
        std::printf("{\"ok\":true,\"root\":%s,\"devices\":%s}\n", json_string(utf8(root)).c_str(),
                    list.c_str());
        return 0;
    }
    return usage();
}
