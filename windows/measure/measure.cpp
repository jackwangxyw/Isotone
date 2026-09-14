// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "measure.h"

#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace measure {

void die(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::exit(2);
}

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
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

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string json_number(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

std::vector<std::string> utf8_args(int argc, wchar_t** argv) {
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.push_back(narrow(argv[i]));
    return out;
}

std::vector<const Endpoint*> match_endpoints(const std::vector<Endpoint>& list, const std::string& query) {
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(query);
    for (const Endpoint& e : list) {
        if (lower(e.id) == q) return {&e};
    }
    std::vector<const Endpoint*> found;
    for (const Endpoint& e : list) {
        if (lower(e.id).find(q) != std::string::npos) found.push_back(&e);
    }
    if (!found.empty()) return found;
    for (const Endpoint& e : list) {
        if (lower(e.description).find(q) != std::string::npos ||
            lower(e.name).find(q) != std::string::npos) {
            found.push_back(&e);
        }
    }
    return found;
}

bool supported(const StreamFormat& f) {
    if (f.channels == 0) return false;
    const uint32_t bytes = f.frame_bytes / f.channels;
    return (f.is_float && f.bits == 32 && bytes == 4) || (!f.is_float && f.bits == 16 && bytes == 2) ||
           (!f.is_float && f.bits == 24 && bytes == 3) || (!f.is_float && (f.bits == 24 || f.bits == 32) && bytes == 4);
}

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
        f.channel_mask = ext->dwChannelMask;
    }
    return f;
}

std::string format_json(const StreamFormat& f) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "{\"sample_rate\": %u, \"channels\": %u, \"sample_format\": \"%s\", \"bits\": %u, "
                  "\"frame_bytes\": %u, \"channel_mask\": %u}",
                  f.sample_rate, f.channels, f.is_float ? "float" : "int", f.bits, f.frame_bytes,
                  f.channel_mask);
    return buf;
}

std::string layout_mismatch(const StreamFormat& render, const StreamFormat& capture, bool allow) {
    if (render.channels == capture.channels || allow) return {};
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "render has %u channels and capture has %u, so capture channel n is not render channel n; "
                  "--allow-channel-mismatch measures anyway",
                  render.channels, capture.channels);
    return buf;
}

void RenderStream::open(IAudioClient* client) {
    client_ = client;
    CHECK(client_->GetMixFormat(&format_), "render GetMixFormat");
    fmt_ = describe(format_);
    if (!supported(fmt_)) {
        // Writing nothing into the buffer would play whatever it held.
        std::fprintf(stderr, "render mix format not supported: %u-bit %s, %u bytes per frame\n", fmt_.bits,
                     fmt_.is_float ? "float" : "int", fmt_.frame_bytes);
        std::exit(2);
    }
    CHECK(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /* 200 ms */, 0, format_,
                              nullptr),
          "render Initialize");
    CHECK(client_->GetBufferSize(&buffer_frames_), "render GetBufferSize");
    // Keep only about 50 ms queued, so a new frequency reaches the endpoint
    // within that of being asked for rather than after a full 200 ms buffer
    // of the old one, which ate most of the settle time.
    lead_frames_ = std::min(buffer_frames_, fmt_.sample_rate / 20);
    CHECK(client_->GetService(__uuidof(IAudioRenderClient),
                              reinterpret_cast<void**>(&render_)),
          "GetService IAudioRenderClient");
}

void RenderStream::start() { CHECK(client_->Start(), "render Start"); }
void RenderStream::stop()  { if (client_) client_->Stop(); }

void RenderStream::pump(double frequency, double amplitude, double* phase) {
    UINT32 padding = 0;
    if (FAILED(client_->GetCurrentPadding(&padding))) {
        ++errors_;
        return;
    }
    // An empty queue once the tone has started is an underrun: the endpoint
    // played a gap, which splices the sine.
    if (primed_ && padding == 0) ++underruns_;
    if (padding >= lead_frames_) return;
    const UINT32 free_frames = lead_frames_ - padding;
    primed_ = true;

    BYTE* data = nullptr;
    if (FAILED(render_->GetBuffer(free_frames, &data))) {
        ++errors_;
        return;
    }

    const double step = 2.0 * kPi * frequency / fmt_.sample_rate;
    for (UINT32 n = 0; n < free_frames; ++n) {
        const double v = amplitude * std::sin(*phase);
        *phase += step;
        if (*phase > 2.0 * kPi) *phase -= 2.0 * kPi;
        for (uint32_t c = 0; c < fmt_.channels; ++c) {
            const double g = c < gains_.size() ? gains_[c] : 1.0;
            write_sample(data, (static_cast<size_t>(n) * fmt_.channels + c), v * g);
        }
    }
    if (FAILED(render_->ReleaseBuffer(free_frames, 0))) {
        ++errors_;
        return;
    }
    frames_ += free_frames;
}

RenderStream::~RenderStream() {
    if (render_) render_->Release();
    if (client_) client_->Release();
    if (format_) CoTaskMemFree(format_);
}

void RenderStream::write_sample(BYTE* base, size_t index, double v) {
    const uint32_t bytes = fmt_.frame_bytes / fmt_.channels;
    const double clamped = std::clamp(v, -1.0, 1.0);
    if (fmt_.is_float && fmt_.bits == 32) {
        reinterpret_cast<float*>(base)[index] = static_cast<float>(v);
    } else if (bytes == 2) {
        reinterpret_cast<int16_t*>(base)[index] = static_cast<int16_t>(clamped * 32767.0);
    } else if (bytes == 3) {
        const int32_t s = static_cast<int32_t>(clamped * 8388607.0);
        const BYTE* p = reinterpret_cast<const BYTE*>(&s);
        std::memcpy(base + index * 3, p, 3);   // little-endian: the low three bytes
    } else if (bytes == 4) {
        reinterpret_cast<int32_t*>(base)[index] = static_cast<int32_t>(clamped * 2147483647.0);
    }
}

void CaptureStream::open(IAudioClient* client, bool loopback) {
    client_ = client;
    CHECK(client_->GetMixFormat(&format_), "capture GetMixFormat");
    fmt_ = describe(format_);
    if (!supported(fmt_)) {
        std::fprintf(stderr, "capture mix format not supported: %u-bit %s, %u bytes per frame\n", fmt_.bits,
                     fmt_.is_float ? "float" : "int", fmt_.frame_bytes);
        std::exit(2);
    }
    const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    CHECK(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, format_, nullptr),
          "capture Initialize");
    CHECK(client_->GetService(__uuidof(IAudioCaptureClient),
                              reinterpret_cast<void**>(&capture_)),
          "GetService IAudioCaptureClient");
}

void CaptureStream::start() { CHECK(client_->Start(), "capture Start"); }
void CaptureStream::stop()  { if (client_) client_->Stop(); }

void CaptureStream::pump(std::vector<std::vector<float>>* sink) {
    for (;;) {
        UINT32 packet = 0;
        if (FAILED(capture_->GetNextPacketSize(&packet))) {
            ++errors_;
            return;
        }
        if (packet == 0) return;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
            ++errors_;
            return;
        }
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities_;

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
        if (FAILED(capture_->ReleaseBuffer(frames))) {
            ++errors_;
            return;
        }
        frames_ += frames;
    }
}

CaptureStream::~CaptureStream() {
    if (capture_) capture_->Release();
    if (client_) client_->Release();
    if (format_) CoTaskMemFree(format_);
}

float CaptureStream::read_sample(const BYTE* base, size_t index) const {
    const uint32_t bytes = fmt_.frame_bytes / fmt_.channels;
    if (fmt_.is_float && fmt_.bits == 32) {
        return reinterpret_cast<const float*>(base)[index];
    }
    if (bytes == 2) {
        return reinterpret_cast<const int16_t*>(base)[index] / 32768.0f;
    }
    if (bytes == 3) {
        const BYTE* p = base + index * 3;
        int32_t s = static_cast<int32_t>(uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16);
        if (s & 0x800000) s -= 0x1000000;
        return static_cast<float>(s / 8388608.0);
    }
    if (bytes == 4) {
        return static_cast<float>(reinterpret_cast<const int32_t*>(base)[index] / 2147483648.0);
    }
    return 0.0f;
}

// A single frequency, measured with a Hann-windowed DFT evaluated at exactly that
// frequency rather than at a bin centre. The window suppresses the leakage that
// would otherwise appear when the frequency does not divide evenly into the
// analysis length, so the result is accurate for any frequency. The magnitude
// is the amplitude of the sine; the angle is its phase at the window start.
std::complex<double> dft_at(const std::vector<float>& x, double frequency, double sample_rate) {
    const size_t n = x.size();
    if (n < kMinWindowFrames) return 0.0;

    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / sample_rate;
        re += x[i] * w * std::cos(phase);
        im -= x[i] * w * std::sin(phase);
        wsum += w;
    }
    return {2.0 * re / wsum, 2.0 * im / wsum};
}

// RMS of what is left of `x` once the sine dft_at found is taken out. A clean
// capture leaves noise; a splice of two out-of-phase pieces leaves a lot.
double residual_rms(const std::vector<float>& x, std::complex<double> fit, double frequency, double sample_rate) {
    if (x.empty()) return 0.0;
    const double a = std::abs(fit), p = std::arg(fit);
    double sum = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double r = x[i] - a * std::cos(2.0 * kPi * frequency * static_cast<double>(i) / sample_rate + p);
        sum += r * r;
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

uint64_t frames_required(double seconds, uint32_t sample_rate) {
    const double frames = std::round(seconds * sample_rate);
    return std::max<uint64_t>(std::isfinite(frames) && frames > 0.0 ? static_cast<uint64_t>(frames) : 0,
                              kMinWindowFrames);
}

WindowResult take_window(RenderStream& render, CaptureStream& capture, const WindowPlan& plan,
                         double* phase, const std::function<void()>& tick) {
    const uint32_t sample_rate = capture.format().sample_rate;
    // A capture that falls behind gets this long past the window to catch up.
    constexpr int kCatchUpTicks = 50;
    WindowResult w;
    for (;;) {
        ++w.attempts;
        const uint32_t errors_before = render.errors() + capture.errors();
        const auto errored = [&] { return render.errors() + capture.errors() != errors_before; };

        // Settle: keep the tone running but throw the capture away, so the
        // measurement window is clear of the path latency and of any transient
        // from the filter that is being measured. Glitches are counted from
        // 100 ms before the window, since one reaches the capture only after
        // the path's latency.
        uint32_t before = capture.discontinuities() + render.underruns();
        for (int t = 0; t < plan.settle_ticks && !errored(); ++t) {
            if (t == plan.settle_ticks - 10) before = capture.discontinuities() + render.underruns();
            render.pump(plan.frequency, plan.amplitude, phase);
            capture.pump(nullptr);
            tick();
        }

        // The window: at least measure_ticks, and until it holds the frames it
        // needs. A failed call ends it, since the attempt already failed.
        w.captured.clear();
        const uint64_t rendered_before = render.frames();
        const uint64_t captured_before = capture.frames();
        for (int t = 0; !errored(); ++t) {
            const bool full = capture.frames() - captured_before >= plan.frames_required;
            if (t >= plan.measure_ticks && (full || t >= plan.measure_ticks + kCatchUpTicks)) break;
            render.pump(plan.frequency, plan.amplitude, phase);
            capture.pump(&w.captured);
            tick();
        }
        w.frames_rendered = render.frames() - rendered_before;
        w.frames_captured = capture.frames() - captured_before;
        const uint32_t attempt_errors = render.errors() + capture.errors() - errors_before;
        w.stream_errors += attempt_errors;
        w.stream_error = attempt_errors != 0;
        w.short_capture = w.frames_captured < plan.frames_required;

        w.glitches = capture.discontinuities() + render.underruns() - before;
        // A splice WASAPI does not flag: measured on the cables, a window whose
        // level read 0.2 dB low on every channel had no discontinuity and no
        // underrun, but a residual of -7 dB against -99 dB in a clean one.
        // More than -40 dB of residual relative to the fitted sine, on any
        // channel carrying the tone, counts as a glitch.
        for (uint32_t c = 0; c < w.captured.size(); ++c) {
            const std::complex<double> fit = dft_at(w.captured[c], plan.frequency, sample_rate);
            const double a = std::abs(fit);
            if (a > kCarryingAmplitude &&
                residual_rms(w.captured[c], fit, plan.frequency, sample_rate) > 0.01 * a / std::sqrt(2.0)) {
                ++w.glitches;
                break;
            }
        }
        w.glitched = w.glitches != 0;
        if (!w.failed() || static_cast<int>(w.attempts) >= plan.max_attempts) break;
    }
    return w;
}

ChannelResults analyse(const WindowResult& window, uint32_t channels, double frequency,
                       double sample_rate, double amplitude, int phase_reference) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    ChannelResults r;
    r.magnitude_db.assign(channels, kNaN);
    r.phase_deg.assign(channels, kNaN);
    r.residual_db.assign(channels, kNaN);
    // A window whose streams failed or that came up short measured nothing; its
    // zeros must not read as silence.
    if (!window.measured()) return r;

    std::vector<std::complex<double>> fits(channels, 0.0);
    for (uint32_t c = 0; c < channels; ++c) {
        double db = -200.0;
        double residual_db = -200.0;
        if (c < window.captured.size() && !window.captured[c].empty()) {
            fits[c] = dft_at(window.captured[c], frequency, sample_rate);
            const double a = std::abs(fits[c]);
            db = a > 0.0 ? 20.0 * std::log10(a) : -200.0;
            const double rms = residual_rms(window.captured[c], fits[c], frequency, sample_rate);
            residual_db = rms > 0.0 ? 20.0 * std::log10(rms / (amplitude / std::sqrt(2.0))) : -200.0;
        }
        r.magnitude_db[c] = db;
        r.residual_db[c] = residual_db;
    }

    const auto carrying = [&](int c) { return std::abs(fits[static_cast<size_t>(c)]) > kCarryingAmplitude; };
    int ref = phase_reference;
    if (ref < 0) {
        if (channels > 0 && carrying(0)) {
            ref = 0;
        } else {
            for (uint32_t c = 0; c < channels; ++c) {
                if (carrying(static_cast<int>(c)) && (ref < 0 || std::abs(fits[c]) > std::abs(fits[static_cast<size_t>(ref)]))) {
                    ref = static_cast<int>(c);
                }
            }
        }
    }
    if (ref < 0 || static_cast<uint32_t>(ref) >= channels || !carrying(ref)) return r;
    r.phase_reference = ref;
    // All channels come from one capture stream, so they share a clock: a delay
    // of d seconds on channel c reads as -360 * f * d, an inverted channel as 180.
    for (uint32_t c = 0; c < channels; ++c) {
        if (!carrying(static_cast<int>(c))) continue;
        r.phase_deg[c] = std::arg(fits[c] * std::conj(fits[static_cast<size_t>(ref)])) * 180.0 / kPi;
    }
    return r;
}

bool MeasureReport::ok() const {
    for (const FrequencyResult& f : results) {
        if (f.window.failed()) return false;
    }
    return true;
}

int exit_code(const MeasureReport& report) { return report.ok() ? 0 : kExitWindowFailed; }

void write_json(std::FILE* out, const MeasureReport& report) {
    const uint32_t channels = report.capture.channels;
    const size_t n = report.results.size();
    std::fprintf(out, "{\n");
    if (!report.label.empty()) std::fprintf(out, "  \"label\": %s,\n", json_string(report.label).c_str());
    std::fprintf(out, "  \"ok\": %s,\n", report.ok() ? "true" : "false");
    std::fprintf(out, "  \"render_id\": %s,\n  \"capture_id\": %s,\n", json_string(report.render_id).c_str(),
                 json_string(report.capture_id).c_str());
    std::fprintf(out, "  \"render_rate\": %u,\n  \"capture_rate\": %u,\n  \"channels\": %u,\n",
                 report.render.sample_rate, report.capture.sample_rate, channels);
    std::fprintf(out, "  \"render_format\": %s,\n", format_json(report.render).c_str());
    std::fprintf(out, "  \"capture_format\": %s,\n", format_json(report.capture).c_str());
    std::fprintf(out, "  \"loopback\": %s,\n", report.loopback ? "true" : "false");
    std::fprintf(out, "  \"amplitude\": %s,\n", json_number(report.amplitude).c_str());
    std::fprintf(out, "  \"frames_required\": %llu,\n", static_cast<unsigned long long>(report.frames_required));
    std::fprintf(out, "  \"stream_errors_total\": %u,\n", report.stream_errors);

    const auto row = [&](const char* name, const std::function<std::string(const FrequencyResult&)>& value) {
        std::fprintf(out, "  \"%s\": [", name);
        for (size_t i = 0; i < n; ++i) std::fprintf(out, "%s%s", i ? ", " : "", value(report.results[i]).c_str());
        std::fprintf(out, "],\n");
    };
    const auto count = [](uint64_t v) { return std::to_string(v); };
    row("frequencies", [&](const FrequencyResult& f) { return json_number(f.frequency); });
    row("glitches", [&](const FrequencyResult& f) { return count(f.window.glitches); });
    row("attempts", [&](const FrequencyResult& f) { return count(f.window.attempts); });
    row("stream_errors", [&](const FrequencyResult& f) { return count(f.window.stream_errors); });
    row("frames_rendered", [&](const FrequencyResult& f) { return count(f.window.frames_rendered); });
    row("frames_captured", [&](const FrequencyResult& f) { return count(f.window.frames_captured); });
    row("failed", [&](const FrequencyResult& f) { return std::string(f.window.failed() ? "true" : "false"); });

    std::fprintf(out, "  \"failed_windows\": [");
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        const FrequencyResult& f = report.results[i];
        if (!f.window.failed()) continue;
        std::string reasons;
        const auto reason = [&](bool on, const char* name) {
            if (!on) return;
            reasons += (reasons.empty() ? "\"" : ", \"") + std::string(name) + "\"";
        };
        reason(f.window.stream_error, "stream_error");
        reason(f.window.short_capture, "short_capture");
        reason(f.window.glitched, "glitch");
        std::fprintf(out, "%s\n    {\"index\": %zu, \"frequency\": %s, \"reasons\": [%s]}", first ? "" : ",", i,
                     json_number(f.frequency).c_str(), reasons.c_str());
        first = false;
    }
    std::fprintf(out, "%s],\n", first ? "" : "\n  ");

    row("phase_reference", [&](const FrequencyResult& f) {
        return f.channels.phase_reference < 0 ? std::string("null") : count(static_cast<uint64_t>(f.channels.phase_reference));
    });

    const auto matrix = [&](const char* name, std::vector<double> ChannelResults::*field, bool last) {
        std::fprintf(out, "  \"%s\": [\n", name);
        for (uint32_t c = 0; c < channels; ++c) {
            std::fprintf(out, "    [");
            for (size_t i = 0; i < n; ++i) {
                const std::vector<double>& v = report.results[i].channels.*field;
                std::fprintf(out, "%s%s", i ? ", " : "", json_number(c < v.size() ? v[c] : std::nan("")).c_str());
            }
            std::fprintf(out, "]%s\n", c + 1 < channels ? "," : "");
        }
        std::fprintf(out, "  ]%s\n", last ? "" : ",");
    };
    matrix("magnitude_db", &ChannelResults::magnitude_db, false);
    matrix("phase_deg", &ChannelResults::phase_deg, false);
    matrix("residual_db", &ChannelResults::residual_db, true);
    std::fprintf(out, "}\n");
}

}  // namespace measure
