// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-measure: plays test tones to a Windows audio endpoint and measures
// what comes back, so that claims about audio can be backed by numbers rather
// than by listening (plan section 10).
//
// Measurement method is a stepped sine. For each frequency: play it, let the
// path settle, then capture a window and take a windowed single-frequency DFT of
// it. That gives the amplitude at exactly that frequency with excellent
// rejection of everything else, and needs no FFT and no deconvolution. It is
// slower than a swept sine and far easier to trust.
//
// The capture side is a real capture endpoint, not WASAPI loopback. With
// VB-Audio Virtual Cable, audio rendered to "CABLE Input" reappears on the
// "CABLE Output" capture device, so the measurement is unambiguously downstream
// of every effect on the render endpoint. WASAPI loopback taps the engine at a
// point that is not documented to be post-APO, which is exactly the thing under
// test here.

#include "measure.h"

#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace measure;

// functiondiscoverykeys_devpkey.h needs DEFINE_PROPERTYKEY, which the SDK only
// exposes in a particular header order that is easy to get wrong. Only two keys
// are needed, so they are written out. These are the same GUID/index pairs the
// endpoint's registry Properties subkey uses:
//   {b3f8fa53-...},6  the device name, e.g. "VB-Audio Virtual Cable"
//   {a45c254e-...},2  the endpoint name, e.g. "CABLE Input"
const PROPERTYKEY kKeyInterfaceFriendlyName = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};
const PROPERTYKEY kKeyDeviceDesc = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

std::vector<Endpoint> enumerate(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    std::vector<Endpoint> out;
    IMMDeviceCollection* collection = nullptr;
    CHECK(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
          "EnumAudioEndpoints");

    UINT count = 0;
    CHECK(collection->GetCount(&count), "GetCount");
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) continue;

        Endpoint e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            e.id = narrow(id);
            CoTaskMemFree(id);
        }

        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kKeyInterfaceFriendlyName, &v)) &&
                v.vt == VT_LPWSTR) {
                e.name = narrow(v.pwszVal);
            }
            PropVariantClear(&v);
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kKeyDeviceDesc, &v)) && v.vt == VT_LPWSTR) {
                e.description = narrow(v.pwszVal);
            }
            PropVariantClear(&v);
            props->Release();
        }
        out.push_back(e);
        device->Release();
    }
    collection->Release();
    return out;
}

IMMDevice* open_by_id(IMMDeviceEnumerator* enumerator, const std::string& id) {
    IMMDevice* device = nullptr;
    const std::wstring wid = widen(id);
    if (FAILED(enumerator->GetDevice(wid.c_str(), &device))) {
        std::fprintf(stderr, "no such endpoint: %s\n", id.c_str());
        std::exit(2);
    }
    return device;
}

IAudioClient* activate(IMMDevice* device, const char* what) {
    IAudioClient* client = nullptr;
    CHECK(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client)),
          what);
    return client;
}

// Resolves a user-supplied string to an endpoint id. Accepts a full id, a bare
// GUID in braces, or a case-insensitive substring of the id, description or
// name, when exactly one endpoint matches.
std::string resolve(const std::vector<Endpoint>& list, const std::string& query) {
    const std::vector<const Endpoint*> found = match_endpoints(list, query);
    if (found.size() == 1) return found[0]->id;
    if (found.empty()) {
        std::fprintf(stderr, "no endpoint matching '%s'\n", query.c_str());
        std::exit(2);
    }
    std::fprintf(stderr, "ambiguous endpoint '%s' matches %zu devices:\n", query.c_str(), found.size());
    for (const Endpoint* e : found) {
        std::fprintf(stderr, "  %-24s %-26s %s\n", e->description.c_str(), e->name.c_str(), e->id.c_str());
    }
    std::exit(2);
}

std::vector<double> default_frequencies() {
    // Thirty-one log-spaced points from 20 Hz to 20 kHz, the usual third-octave
    // centres, which is plenty to see the shape of any EQ curve.
    std::vector<double> f;
    for (int i = 0; i < 31; ++i) {
        f.push_back(20.0 * std::pow(10.0, 3.0 * i / 30.0));
    }
    return f;
}

std::vector<double> parse_frequencies(const std::string& csv) {
    std::vector<double> out;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const std::string piece =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty()) {
            out.push_back(std::strtod(piece.c_str(), nullptr));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

int cmd_list(IMMDeviceEnumerator* enumerator, bool json) {
    const std::vector<Endpoint> render = enumerate(enumerator, eRender);
    const std::vector<Endpoint> capture = enumerate(enumerator, eCapture);

    if (json) {
        std::printf("{\n  \"render\": [\n");
        for (size_t i = 0; i < render.size(); ++i) {
            std::printf("    {\"id\": %s, \"name\": %s, \"description\": %s}%s\n",
                        json_string(render[i].id).c_str(), json_string(render[i].name).c_str(),
                        json_string(render[i].description).c_str(), i + 1 < render.size() ? "," : "");
        }
        std::printf("  ],\n  \"capture\": [\n");
        for (size_t i = 0; i < capture.size(); ++i) {
            std::printf("    {\"id\": %s, \"name\": %s, \"description\": %s}%s\n",
                        json_string(capture[i].id).c_str(), json_string(capture[i].name).c_str(),
                        json_string(capture[i].description).c_str(), i + 1 < capture.size() ? "," : "");
        }
        std::printf("  ]\n}\n");
        return 0;
    }

    std::printf("RENDER endpoints (active):\n");
    for (const Endpoint& e : render) {
        std::printf("  %-24s %-26s %s\n", e.description.c_str(), e.name.c_str(), e.id.c_str());
    }
    std::printf("\nCAPTURE endpoints (active):\n");
    for (const Endpoint& e : capture) {
        std::printf("  %-24s %-26s %s\n", e.description.c_str(), e.name.c_str(), e.id.c_str());
    }
    return 0;
}

struct MeasureOptions {
    std::string render_query;
    std::string capture_query;
    bool   loopback   = false;
    double amplitude  = 0.25;
    double settle_s   = 0.30;
    double measure_s  = 0.30;
    std::vector<double> frequencies;
    std::vector<double> channel_gains;
    bool   json       = false;
    std::string label;
    bool   allow_channel_mismatch = false;
    int    phase_reference = -1;   // -1: channel 0, or the strongest when channel 0 has no tone
};

int cmd_measure(IMMDeviceEnumerator* enumerator, const MeasureOptions& opt) {
    const std::vector<Endpoint> render_list = enumerate(enumerator, eRender);
    const std::vector<Endpoint> capture_list =
        enumerate(enumerator, opt.loopback ? eRender : eCapture);

    const std::string render_id = resolve(render_list, opt.render_query);
    const std::string capture_id =
        opt.capture_query.empty() ? render_id : resolve(capture_list, opt.capture_query);

    IMMDevice* render_device = open_by_id(enumerator, render_id);
    IMMDevice* capture_device = open_by_id(enumerator, capture_id);

    RenderStream render;
    CaptureStream capture;
    render.open(activate(render_device, "Activate render IAudioClient"));
    render.set_channel_gains(opt.channel_gains);
    capture.open(activate(capture_device, "Activate capture IAudioClient"), opt.loopback);

    const StreamFormat rf = render.format();
    const StreamFormat cf = capture.format();

    if (!opt.json) {
        std::fprintf(stderr, "render  %s  %u Hz %u ch %s%u mask 0x%x\n", render_id.c_str(), rf.sample_rate,
                     rf.channels, rf.is_float ? "float" : "int", rf.bits, rf.channel_mask);
        std::fprintf(stderr, "capture %s  %u Hz %u ch %s%u mask 0x%x%s\n", capture_id.c_str(),
                     cf.sample_rate, cf.channels, cf.is_float ? "float" : "int", cf.bits, cf.channel_mask,
                     opt.loopback ? " (loopback)" : "");
    }
    const std::string mismatch = layout_mismatch(rf, cf, opt.allow_channel_mismatch);
    if (!mismatch.empty()) {
        std::fprintf(stderr, "%s\n", mismatch.c_str());
        std::exit(2);
    }
    if (opt.phase_reference >= static_cast<int>(cf.channels)) {
        std::fprintf(stderr, "--phase-ref %d: capture has %u channels\n", opt.phase_reference, cf.channels);
        std::exit(2);
    }

    render.start();
    capture.start();

    MeasureReport report;
    report.label = opt.label;
    report.render_id = render_id;
    report.capture_id = capture_id;
    report.render = rf;
    report.capture = cf;
    report.loopback = opt.loopback;
    report.amplitude = opt.amplitude;
    report.frames_required = frames_required(opt.measure_s, cf.sample_rate);

    WindowPlan plan;
    plan.amplitude = opt.amplitude;
    plan.settle_ticks = static_cast<int>(opt.settle_s * 1000.0 / 10.0);
    plan.measure_ticks = static_cast<int>(opt.measure_s * 1000.0 / 10.0);
    plan.frames_required = report.frames_required;

    double phase = 0.0;
    for (double freq : opt.frequencies) {
        plan.frequency = freq;
        FrequencyResult result;
        result.frequency = freq;
        result.window = take_window(render, capture, plan, &phase, [] { Sleep(10); });
        result.channels = analyse(result.window, cf.channels, freq, cf.sample_rate, opt.amplitude,
                                  opt.phase_reference);
        result.window.captured.clear();
        report.results.push_back(result);

        if (!opt.json) {
            const WindowResult& w = result.window;
            const ChannelResults& r = result.channels;
            std::printf("%10.2f Hz", freq);
            double worst_residual = -200.0;
            for (uint32_t c = 0; c < cf.channels; ++c) {
                if (std::isnan(r.magnitude_db[c])) {
                    std::printf("   ch%u       -- dB", c);
                } else {
                    std::printf("   ch%u %+8.3f dB", c, r.magnitude_db[c]);
                }
                if (static_cast<int>(c) != r.phase_reference) {
                    if (std::isnan(r.phase_deg[c])) {
                        std::printf("      -- deg");
                    } else {
                        std::printf(" %+7.2f deg", r.phase_deg[c]);
                    }
                }
                if (!std::isnan(r.residual_db[c])) worst_residual = std::max(worst_residual, r.residual_db[c]);
            }
            std::printf("   residual %+.1f dB", worst_residual);
            if (r.phase_reference >= 0) {
                std::printf("   phase re ch%d", r.phase_reference);
            } else {
                std::printf("   no phase reference");
            }
            if (w.stream_errors != 0) std::printf("   STREAM ERRORS x%u", w.stream_errors);
            if (w.short_capture) {
                std::printf("   SHORT %llu of %llu frames", static_cast<unsigned long long>(w.frames_captured),
                            static_cast<unsigned long long>(plan.frames_required));
            }
            if (w.glitches != 0) std::printf("   GLITCH x%u", w.glitches);
            if (w.attempts > 1) std::printf("   after %u attempts", w.attempts);
            if (w.failed()) std::printf("   FAILED");
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    render.stop();
    capture.stop();
    render_device->Release();
    capture_device->Release();
    report.stream_errors = render.errors() + capture.errors();

    if (opt.json) write_json(stdout, report);
    if (!report.ok()) {
        size_t failed = 0;
        for (const FrequencyResult& f : report.results) failed += f.window.failed() ? 1 : 0;
        std::fprintf(stderr, "%zu of %zu windows failed after retakes; %u stream errors\n", failed,
                     report.results.size(), report.stream_errors);
    }
    return exit_code(report);
}

int cmd_play(IMMDeviceEnumerator* enumerator, const std::string& query, double freq,
             double seconds, double amplitude) {
    const std::vector<Endpoint> list = enumerate(enumerator, eRender);
    const std::string id = resolve(list, query);
    IMMDevice* device = open_by_id(enumerator, id);

    RenderStream render;
    render.open(activate(device, "Activate render IAudioClient"));
    render.start();
    double phase = 0.0;
    const int ticks = static_cast<int>(seconds * 1000.0 / 10.0);
    for (int t = 0; t < ticks; ++t) {
        render.pump(freq, amplitude, &phase);
        Sleep(10);
    }
    render.stop();
    device->Release();
    std::fprintf(stderr, "played %.1f Hz for %.2f s to %s\n", freq, seconds, id.c_str());
    if (render.errors() != 0) {
        std::fprintf(stderr, "%u stream errors\n", render.errors());
        return 2;
    }
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "isotone-measure -- audio measurement for Isotone\n"
        "\n"
        "  list [--json]\n"
        "      Show active render and capture endpoints.\n"
        "\n"
        "  measure --render <id|substring> (--capture <id|substring> | --loopback) [options]\n"
        "      Play a stepped sine and report the measured level at each frequency,\n"
        "      and each channel's phase relative to a reference channel.\n"
        "      --loopback         capture via WASAPI loopback on the render endpoint\n"
        "                         instead of from a separate capture device\n"
        "      --freqs a,b,c      frequencies in Hz (default: 31 third-octave points)\n"
        "      --amplitude x      tone amplitude, 0..1 (default 0.25)\n"
        "      --channel-gains a,b,c  per render channel, multiplies the amplitude\n"
        "                         (default 1 for every channel)\n"
        "      --settle s         seconds to discard before each measurement (default 0.30)\n"
        "      --window s         seconds to measure (default 0.30)\n"
        "      --phase-ref n      capture channel phases are relative to (default: channel 0,\n"
        "                         or the strongest channel when channel 0 is below -80 dBFS)\n"
        "      --allow-channel-mismatch  measure when render and capture channel counts differ\n"
        "      A window with a stream error, fewer frames than --window, a capture\n"
        "      discontinuity, a render underrun near it, or a residual above -40 dB re its\n"
        "      fitted sine is taken again, up to 3 times. A channel below -80 dBFS, or\n"
        "      any channel when the reference is, has no phase (null in JSON); a window\n"
        "      with a stream error or too few frames has no numbers at all.\n"
        "      --label text       copied into the JSON output\n"
        "      --json             machine-readable output\n"
        "      Exit status: 0 every window passed, 3 a window still failed after its\n"
        "      retakes (listed in failed_windows), 2 an error before measuring, 1 usage.\n"
        "\n"
        "  play --render <id|substring> [--freq hz] [--seconds s] [--amplitude x]\n"
        "      Play a tone. Useful for making Windows instantiate an APO.\n");
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    // UTF-8 from here on: a label, path or endpoint name reaches the JSON as
    // written, where main's ANSI arguments would not.
    const std::vector<std::string> argv = utf8_args(argc, wargv);
    if (argc < 2) {
        usage();
        return 1;
    }

    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

    IMMDeviceEnumerator* enumerator = nullptr;
    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)),
          "CoCreateInstance MMDeviceEnumerator");

    const std::string command = argv[1];
    int rc = 1;

    auto arg = [&](const char* name, const char* fallback = nullptr) -> const char* {
        for (int i = 2; i + 1 < argc; ++i) {
            if (argv[i] == name) return argv[i + 1].c_str();
        }
        return fallback;
    };
    auto flag = [&](const char* name) {
        for (int i = 2; i < argc; ++i) {
            if (argv[i] == name) return true;
        }
        return false;
    };

    if (command == "list") {
        rc = cmd_list(enumerator, flag("--json"));
    } else if (command == "measure") {
        MeasureOptions opt;
        const char* r = arg("--render");
        const char* phase_ref = arg("--phase-ref");
        char* phase_ref_end = nullptr;
        const long phase_ref_value = phase_ref ? std::strtol(phase_ref, &phase_ref_end, 10) : -1;
        if (r == nullptr) {
            std::fprintf(stderr, "measure needs --render\n");
        } else if (arg("--capture") == nullptr && !flag("--loopback")) {
            // Without either, the render endpoint would be opened as a capture
            // client, which fails.
            std::fprintf(stderr, "measure needs --capture, or --loopback to capture the render endpoint\n");
        } else if (phase_ref != nullptr && (*phase_ref == '\0' || *phase_ref_end != '\0' || phase_ref_value < 0)) {
            std::fprintf(stderr, "--phase-ref '%s' is not a channel number\n", phase_ref);
        } else if (!settle_is_enough(std::strtod(arg("--settle", "0.30"), nullptr))) {
            std::fprintf(stderr, "--settle must be at least %.2f s\n", kMinSettleSeconds);
        } else {
            opt.render_query  = r;
            opt.capture_query = arg("--capture", "");
            opt.loopback      = flag("--loopback");
            opt.json          = flag("--json");
            opt.label         = arg("--label", "");
            opt.amplitude     = std::strtod(arg("--amplitude", "0.25"), nullptr);
            opt.settle_s      = std::strtod(arg("--settle", "0.30"), nullptr);
            opt.measure_s     = std::strtod(arg("--window", "0.30"), nullptr);
            const char* freqs = arg("--freqs");
            opt.frequencies   = freqs ? parse_frequencies(freqs) : default_frequencies();
            const char* gains = arg("--channel-gains");
            if (gains) opt.channel_gains = parse_frequencies(gains);
            opt.allow_channel_mismatch = flag("--allow-channel-mismatch");
            opt.phase_reference = static_cast<int>(phase_ref_value);
            rc = cmd_measure(enumerator, opt);
        }
    } else if (command == "play") {
        const char* r = arg("--render");
        if (r == nullptr) {
            std::fprintf(stderr, "play needs --render\n");
        } else {
            rc = cmd_play(enumerator, r, std::strtod(arg("--freq", "1000"), nullptr),
                          std::strtod(arg("--seconds", "1.0"), nullptr),
                          std::strtod(arg("--amplitude", "0.25"), nullptr));
        }
    } else {
        usage();
    }

    enumerator->Release();
    CoUninitialize();
    return rc;
}
