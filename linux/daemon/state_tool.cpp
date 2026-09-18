// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-state: what windows/shmtool's isotone-shm is on Windows. It puts a
// state into a sink's live shared region or into its saved-state file, prints
// what is there, and drains the post-EQ ring, so the daemon can be driven and
// inspected without a UI.
//
//   isotone-state show    --sink <name>
//   isotone-state set     --sink <name> [--band f,gain,q] [--preamp dB] [--bypass]
//   isotone-state save    --sink <name> [--dir D] [--band f,gain,q] ...
//   isotone-state capture --sink <name> [--seconds s]
//
// set writes the live region, which is the path the UI takes for every edit.
// save writes the file the daemon seeds a fresh region from, which is the path a
// sink takes when it comes up before any UI runs. capture reads the ring the
// UI's spectrum reads.

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "isotone/audio_ring.h"
#include "isotone/param_block.h"
#include "isotone/types.h"
#include "persisted_state.h"
#include "shared_region.h"

using namespace isotone;

namespace {

void usage() {
    std::printf(
        "isotone-state <show|set|save|capture> --sink <node.name> [options]\n"
        "\n"
        "  --band <fc,gain_db,q>   a peaking band; repeatable\n"
        "  --preamp <dB>           preamp\n"
        "  --bypass                bypass the bands\n"
        "  --dir <dir>             saved-state directory (save and show)\n"
        "  --seconds <s>           how long capture drains the ring (default 2)\n"
        "\n"
        "ISOTONE_CAPTURE_RAW=<path> writes capture's left channel there as raw\n"
        "float32, for a measurement to pick the tone out of.\n");
}

bool parse_band(const char* text, Band* out) {
    double fc = 0, gain = 0, q = 0;
    if (std::sscanf(text, "%lf,%lf,%lf", &fc, &gain, &q) != 3) return false;
    out->type = FilterType::Peaking;
    out->fc = fc;
    out->gain_db = gain;
    out->width = q;
    out->width_mode = WidthMode::Q;
    out->enabled = true;
    return true;
}

const char* host_state_name(uint32_t value) {
    switch (static_cast<HostState>(value)) {
        case HostState::NotLoaded: return "not loaded";
        case HostState::Running:   return "running";
        case HostState::Error:     return "error";
    }
    return "?";
}

int show(const std::string& sink, const std::string& dir) {
    const std::string name = posix::region_name(sink);
    posix::SharedRegion region;
    const int error = region.open(name);
    if (error != 0) {
        std::printf("region %s: %s\n", name.c_str(), std::strerror(error));
    } else {
        ParamBlock block{};
        if (!param_block_read(region.params(), &block)) {
            std::printf("region %s: no consistent read\n", name.c_str());
            return 1;
        }
        std::printf("region %s\n", name.c_str());
        std::printf("  host        %s, %u Hz, %u ch, heartbeat %u\n",
                    host_state_name(block.hdr.host_state), block.hdr.sample_rate,
                    block.hdr.channels, block.hdr.host_heartbeat);
        std::printf("  preamp      %.2f dB%s\n", static_cast<double>(block.preamp_db),
                    block.bypass ? "  (bypassed)" : "");
        std::printf("  bands       %u\n", block.band_count);
        for (uint32_t i = 0; i < block.band_count && i < kParamMaxBands; ++i) {
            const ParamBand& b = block.bands[i];
            std::printf("    %2u  %8.1f Hz  %+6.2f dB  width %.3f\n", i, static_cast<double>(b.fc),
                        static_cast<double>(b.gain_db), static_cast<double>(b.width));
        }
    }

    const std::string state_dir = dir.empty() ? posix::persisted_state_dir() : dir;
    const std::string path = posix::persisted_state_path(state_dir, sink);
    ParamBlock saved{};
    switch (posix::read_persisted_state(path, &saved)) {
        case posix::PersistedRead::Absent:
            std::printf("saved  %s: none\n", path.c_str());
            break;
        case posix::PersistedRead::Invalid:
            std::printf("saved  %s: not a block for this build\n", path.c_str());
            break;
        case posix::PersistedRead::Loaded:
            std::printf("saved  %s: %u bands, preamp %.2f dB\n", path.c_str(), saved.band_count,
                        static_cast<double>(saved.preamp_db));
            break;
    }
    return 0;
}

// Drains the region's post-EQ ring, the audio the UI's spectrum reads, and
// reports what arrived. The counterpart of isotone-shm capture on Windows.
int capture(const std::string& sink, double seconds) {
    const std::string name = posix::region_name(sink);
    posix::SharedRegion region;
    const int error = region.open(name);
    if (error != 0) {
        std::fprintf(stderr, "isotone-state: region %s: %s\n", name.c_str(), std::strerror(error));
        return 1;
    }

    constexpr uint32_t kChunk = 4096;
    AudioRingCursor cursor;
    std::vector<float> buffer(static_cast<size_t>(kChunk) * kMaxChannels);
    std::vector<float> left;
    uint32_t channels = 0;
    uint64_t frames = 0;
    double peak = 0.0, sum_squares = 0.0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        uint32_t got_channels = 0;
        const uint32_t got =
            audio_ring_read(region.ring(), kRingCapacityFrames, &cursor, buffer.data(), kChunk, &got_channels);
        if (got == 0 || got_channels == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        channels = got_channels;
        frames += got;
        for (uint32_t i = 0; i < got * got_channels; ++i) {
            const double v = buffer[i];
            peak = std::max(peak, std::abs(v));
            sum_squares += v * v;
        }
        for (uint32_t i = 0; i < got; ++i) left.push_back(buffer[i * got_channels]);
    }

    if (frames == 0) {
        std::printf("no audio in the ring\n");
        return 1;
    }
    const double rms = std::sqrt(sum_squares / static_cast<double>(frames * channels));
    std::printf("frames %llu  channels %u  peak %.6f (%.3f dBFS)  rms %.3f dBFS\n",
                static_cast<unsigned long long>(frames), channels, peak,
                20.0 * std::log10(peak > 0 ? peak : 1e-12), 20.0 * std::log10(rms > 0 ? rms : 1e-12));

    if (const char* path = std::getenv("ISOTONE_CAPTURE_RAW")) {
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "isotone-state: %s: %s\n", path, std::strerror(errno));
            return 1;
        }
        std::fwrite(left.data(), sizeof(float), left.size(), f);
        std::fclose(f);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];

    std::string sink, dir;
    double seconds = 2.0;
    EqState state;
    uint32_t next_id = 1;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--sink" && has_value) {
            sink = argv[++i];
        } else if (arg == "--dir" && has_value) {
            dir = argv[++i];
        } else if (arg == "--seconds" && has_value) {
            seconds = std::strtod(argv[++i], nullptr);
        } else if (arg == "--preamp" && has_value) {
            state.preamp_db = std::strtod(argv[++i], nullptr);
        } else if (arg == "--bypass") {
            state.bypass = true;
        } else if (arg == "--band" && has_value) {
            Band band;
            if (!parse_band(argv[++i], &band)) {
                std::fprintf(stderr, "isotone-state: --band wants fc,gain_db,q\n");
                return 2;
            }
            band.id = next_id++;
            state.bands.push_back(band);
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "isotone-state: unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (sink.empty()) {
        usage();
        return 2;
    }

    if (command == "show") return show(sink, dir);
    if (command == "capture") return capture(sink, seconds);

    ParamBlock wanted{};
    init_param_block(&wanted);
    if (!to_param_block(state, &wanted)) {
        std::fprintf(stderr, "isotone-state: the state does not fit a block\n");
        return 1;
    }

    if (command == "set") {
        const std::string name = posix::region_name(sink);
        posix::SharedRegion region;
        const int error = region.open(name);
        if (error != 0) {
            std::fprintf(stderr, "isotone-state: region %s: %s\n", name.c_str(), std::strerror(error));
            return 1;
        }
        // The parameters only: the header belongs to the daemon.
        param_block_write(region.params(), [&wanted](ParamBlock* live) {
            const ParamBlockHeader header = live->hdr;
            *live = wanted;
            live->hdr = header;
        });
        std::printf("set %s: %zu bands, preamp %.2f dB\n", name.c_str(), state.bands.size(),
                    state.preamp_db);
        return 0;
    }

    if (command == "save") {
        const std::string state_dir = dir.empty() ? posix::persisted_state_dir() : dir;
        const std::string path = posix::persisted_state_path(state_dir, sink);
        const int error = posix::write_persisted_state(path, wanted);
        if (error != 0) {
            std::fprintf(stderr, "isotone-state: %s: %s\n", path.c_str(), std::strerror(error));
            return 1;
        }
        std::printf("saved %s\n", path.c_str());
        return 0;
    }

    usage();
    return 2;
}
