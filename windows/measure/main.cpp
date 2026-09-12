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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

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

void die(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::exit(2);
}

#define CHECK(expr, what)                        \
    do {                                         \
        const HRESULT _hr = (expr);              \
        if (FAILED(_hr)) die((what), _hr);       \
    } while (0)

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct Endpoint {
    std::string id;
    std::string name;        // device name, e.g. "VB-Audio Virtual Cable"
    std::string description; // endpoint name, e.g. "CABLE Input"
};

// Format description reduced to what the tone generator and analyser need.
struct StreamFormat {
    uint32_t sample_rate = 0;
    uint32_t channels    = 0;
    bool     is_float    = false;
    uint32_t bits        = 0;
    uint32_t frame_bytes = 0;
};

StreamFormat describe(const WAVEFORMATEX* wfx) {
    StreamFormat f;
    f.sample_rate = wfx->nSamplesPerSec;
    f.channels    = wfx->nChannels;
    f.bits        = wfx->wBitsPerSample;
    f.frame_bytes = wfx->nBlockAlign;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        f.is_float = true;
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        f.is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return f;
}

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

// Resolves a user-supplied string to an endpoint id. Accepts a full id, a bare
// GUID in braces, or a case-insensitive substring of the description or name.
std::string resolve(const std::vector<Endpoint>& list, const std::string& query) {
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(query);
    for (const Endpoint& e : list) {
        if (lower(e.id) == q) return e.id;
    }
    for (const Endpoint& e : list) {
        if (lower(e.id).find(q) != std::string::npos) return e.id;
    }
    std::string found;
    int matches = 0;
    for (const Endpoint& e : list) {
        if (lower(e.description).find(q) != std::string::npos ||
            lower(e.name).find(q) != std::string::npos) {
            found = e.id;
            ++matches;
        }
    }
    if (matches == 1) return found;
    if (matches > 1) {
        std::fprintf(stderr, "ambiguous endpoint '%s' matches %d devices\n", query.c_str(),
                     matches);
        std::exit(2);
    }
    std::fprintf(stderr, "no endpoint matching '%s'\n", query.c_str());
    std::exit(2);
}

class RenderStream {
public:
    void open(IMMDevice* device) {
        CHECK(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client_)),
              "Activate render IAudioClient");
        CHECK(client_->GetMixFormat(&format_), "render GetMixFormat");
        fmt_ = describe(format_);
        CHECK(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /* 200 ms */, 0, format_,
                                  nullptr),
              "render Initialize");
        CHECK(client_->GetBufferSize(&buffer_frames_), "render GetBufferSize");
        CHECK(client_->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render_)),
              "GetService IAudioRenderClient");
    }

    void start() { CHECK(client_->Start(), "render Start"); }
    void stop()  { if (client_) client_->Stop(); }

    // Writes as many frames of the tone as the buffer has room for.
    void pump(double frequency, double amplitude, double* phase) {
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return;
        const UINT32 free_frames = buffer_frames_ - padding;
        if (free_frames == 0) return;

        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(free_frames, &data))) return;

        const double step = 2.0 * kPi * frequency / fmt_.sample_rate;
        for (UINT32 n = 0; n < free_frames; ++n) {
            const double v = amplitude * std::sin(*phase);
            *phase += step;
            if (*phase > 2.0 * kPi) *phase -= 2.0 * kPi;
            for (uint32_t c = 0; c < fmt_.channels; ++c) {
                write_sample(data, (static_cast<size_t>(n) * fmt_.channels + c), v);
            }
        }
        render_->ReleaseBuffer(free_frames, 0);
    }

    const StreamFormat& format() const { return fmt_; }

    ~RenderStream() {
        if (render_) render_->Release();
        if (client_) client_->Release();
        if (format_) CoTaskMemFree(format_);
    }

private:
    void write_sample(BYTE* base, size_t index, double v) {
        if (fmt_.is_float && fmt_.bits == 32) {
            reinterpret_cast<float*>(base)[index] = static_cast<float>(v);
        } else if (fmt_.bits == 16) {
            const double clamped = std::clamp(v, -1.0, 1.0);
            reinterpret_cast<int16_t*>(base)[index] = static_cast<int16_t>(clamped * 32767.0);
        } else if (fmt_.bits == 32) {
            const double clamped = std::clamp(v, -1.0, 1.0);
            reinterpret_cast<int32_t*>(base)[index] =
                static_cast<int32_t>(clamped * 2147483647.0);
        }
    }

    IAudioClient*       client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    WAVEFORMATEX*       format_ = nullptr;
    StreamFormat        fmt_{};
    UINT32              buffer_frames_ = 0;
};

class CaptureStream {
public:
    void open(IMMDevice* device, bool loopback) {
        CHECK(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client_)),
              "Activate capture IAudioClient");
        CHECK(client_->GetMixFormat(&format_), "capture GetMixFormat");
        fmt_ = describe(format_);
        const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        CHECK(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, format_, nullptr),
              "capture Initialize");
        CHECK(client_->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&capture_)),
              "GetService IAudioCaptureClient");
    }

    void start() { CHECK(client_->Start(), "capture Start"); }
    void stop()  { if (client_) client_->Stop(); }

    // Drains whatever is available. When `sink` is null the audio is discarded,
    // which is how the settling period is skipped.
    void pump(std::vector<std::vector<float>>* sink) {
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture_->GetNextPacketSize(&packet)) || packet == 0) return;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;

            if (sink != nullptr) {
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                sink->resize(fmt_.channels);
                for (UINT32 n = 0; n < frames; ++n) {
                    for (uint32_t c = 0; c < fmt_.channels; ++c) {
                        (*sink)[c].push_back(
                            silent ? 0.0f
                                   : read_sample(data, static_cast<size_t>(n) * fmt_.channels + c));
                    }
                }
            }
            capture_->ReleaseBuffer(frames);
        }
    }

    const StreamFormat& format() const { return fmt_; }

    ~CaptureStream() {
        if (capture_) capture_->Release();
        if (client_) client_->Release();
        if (format_) CoTaskMemFree(format_);
    }

private:
    float read_sample(const BYTE* base, size_t index) const {
        if (fmt_.is_float && fmt_.bits == 32) {
            return reinterpret_cast<const float*>(base)[index];
        }
        if (fmt_.bits == 16) {
            return reinterpret_cast<const int16_t*>(base)[index] / 32768.0f;
        }
        if (fmt_.bits == 32) {
            return static_cast<float>(reinterpret_cast<const int32_t*>(base)[index] /
                                      2147483648.0);
        }
        return 0.0f;
    }

    IAudioClient*        client_  = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    WAVEFORMATEX*        format_  = nullptr;
    StreamFormat         fmt_{};
};

// Amplitude of a single frequency, measured with a Hann-windowed DFT evaluated at
// exactly that frequency rather than at a bin centre. The window suppresses the
// leakage that would otherwise appear when the frequency does not divide evenly
// into the analysis length, so the result is accurate for any frequency.
double amplitude_at(const std::vector<float>& x, double frequency, double sample_rate) {
    const size_t n = x.size();
    if (n < 64) return 0.0;

    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / sample_rate;
        re += x[i] * w * std::cos(phase);
        im -= x[i] * w * std::sin(phase);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

double rms(const std::vector<float>& x) {
    if (x.empty()) return 0.0;
    double sum = 0.0;
    for (float v : x) sum += static_cast<double>(v) * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
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
            std::printf("    {\"id\": \"%s\", \"name\": \"%s\", \"description\": \"%s\"}%s\n",
                        render[i].id.c_str(), render[i].name.c_str(),
                        render[i].description.c_str(), i + 1 < render.size() ? "," : "");
        }
        std::printf("  ],\n  \"capture\": [\n");
        for (size_t i = 0; i < capture.size(); ++i) {
            std::printf("    {\"id\": \"%s\", \"name\": \"%s\", \"description\": \"%s\"}%s\n",
                        capture[i].id.c_str(), capture[i].name.c_str(),
                        capture[i].description.c_str(), i + 1 < capture.size() ? "," : "");
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
    bool   json       = false;
    std::string label;
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
    render.open(render_device);
    capture.open(capture_device, opt.loopback);

    const StreamFormat rf = render.format();
    const StreamFormat cf = capture.format();

    if (!opt.json) {
        std::fprintf(stderr, "render  %s  %u Hz %u ch %s%u\n", render_id.c_str(), rf.sample_rate,
                     rf.channels, rf.is_float ? "float" : "int", rf.bits);
        std::fprintf(stderr, "capture %s  %u Hz %u ch %s%u%s\n", capture_id.c_str(),
                     cf.sample_rate, cf.channels, cf.is_float ? "float" : "int", cf.bits,
                     opt.loopback ? " (loopback)" : "");
    }

    render.start();
    capture.start();

    double phase = 0.0;
    std::vector<std::vector<double>> results;   // [channel][frequency]
    results.resize(cf.channels);

    for (double freq : opt.frequencies) {
        // Settle: keep the tone running but throw the capture away, so the
        // measurement window is clear of the path latency and of any transient
        // from the filter that is being measured.
        const int settle_ticks = static_cast<int>(opt.settle_s * 1000.0 / 10.0);
        for (int t = 0; t < settle_ticks; ++t) {
            render.pump(freq, opt.amplitude, &phase);
            capture.pump(nullptr);
            Sleep(10);
        }

        std::vector<std::vector<float>> captured;
        const int measure_ticks = static_cast<int>(opt.measure_s * 1000.0 / 10.0);
        for (int t = 0; t < measure_ticks; ++t) {
            render.pump(freq, opt.amplitude, &phase);
            capture.pump(&captured);
            Sleep(10);
        }

        for (uint32_t c = 0; c < cf.channels; ++c) {
            double db = -200.0;
            if (c < captured.size() && !captured[c].empty()) {
                const double a = amplitude_at(captured[c], freq, cf.sample_rate);
                db = a > 0.0 ? 20.0 * std::log10(a) : -200.0;
            }
            results[c].push_back(db);
        }

        if (!opt.json) {
            std::printf("%10.2f Hz", freq);
            for (uint32_t c = 0; c < cf.channels; ++c) {
                std::printf("   ch%u %+8.3f dB", c, results[c].back());
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    render.stop();
    capture.stop();
    render_device->Release();
    capture_device->Release();

    if (opt.json) {
        std::printf("{\n");
        if (!opt.label.empty()) std::printf("  \"label\": \"%s\",\n", opt.label.c_str());
        std::printf("  \"render_id\": \"%s\",\n  \"capture_id\": \"%s\",\n", render_id.c_str(),
                    capture_id.c_str());
        std::printf("  \"render_rate\": %u,\n  \"capture_rate\": %u,\n  \"channels\": %u,\n",
                    rf.sample_rate, cf.sample_rate, cf.channels);
        std::printf("  \"amplitude\": %.6f,\n", opt.amplitude);
        std::printf("  \"frequencies\": [");
        for (size_t i = 0; i < opt.frequencies.size(); ++i) {
            std::printf("%s%.6f", i ? ", " : "", opt.frequencies[i]);
        }
        std::printf("],\n  \"magnitude_db\": [\n");
        for (uint32_t c = 0; c < cf.channels; ++c) {
            std::printf("    [");
            for (size_t i = 0; i < results[c].size(); ++i) {
                std::printf("%s%.6f", i ? ", " : "", results[c][i]);
            }
            std::printf("]%s\n", c + 1 < cf.channels ? "," : "");
        }
        std::printf("  ]\n}\n");
    }
    return 0;
}

int cmd_play(IMMDeviceEnumerator* enumerator, const std::string& query, double freq,
             double seconds, double amplitude) {
    const std::vector<Endpoint> list = enumerate(enumerator, eRender);
    const std::string id = resolve(list, query);
    IMMDevice* device = open_by_id(enumerator, id);

    RenderStream render;
    render.open(device);
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
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "isotone-measure -- audio measurement for Isotone\n"
        "\n"
        "  list [--json]\n"
        "      Show active render and capture endpoints.\n"
        "\n"
        "  measure --render <id|substring> [--capture <id|substring>] [options]\n"
        "      Play a stepped sine and report the measured level at each frequency.\n"
        "      --loopback         capture via WASAPI loopback on the render endpoint\n"
        "                         instead of from a separate capture device\n"
        "      --freqs a,b,c      frequencies in Hz (default: 31 third-octave points)\n"
        "      --amplitude x      tone amplitude, 0..1 (default 0.25)\n"
        "      --settle s         seconds to discard before each measurement (default 0.30)\n"
        "      --window s         seconds to measure (default 0.30)\n"
        "      --label text       copied into the JSON output\n"
        "      --json             machine-readable output\n"
        "\n"
        "  play --render <id|substring> [--freq hz] [--seconds s] [--amplitude x]\n"
        "      Play a tone. Useful for making Windows instantiate an APO.\n");
}

}  // namespace

int main(int argc, char** argv) {
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
            if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
        }
        return fallback;
    };
    auto flag = [&](const char* name) {
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], name) == 0) return true;
        }
        return false;
    };

    if (command == "list") {
        rc = cmd_list(enumerator, flag("--json"));
    } else if (command == "measure") {
        MeasureOptions opt;
        const char* r = arg("--render");
        if (r == nullptr) {
            std::fprintf(stderr, "measure needs --render\n");
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
