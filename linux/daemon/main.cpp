// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "daemon.h"
#include "daemon_config.h"

namespace {

void usage() {
    std::printf(
        "isotone-daemon --sink <node.name> [options]\n"
        "\n"
        "  --sink <name>          the hardware sink processed audio is played into.\n"
        "                         Names the shared region and the saved-state file.\n"
        "  --name <name>          node.name of the virtual sink (default isotone)\n"
        "  --description <text>   how the virtual sink is shown (default Isotone)\n"
        "  --state-dir <dir>      saved state directory; default is\n"
        "                         $XDG_CONFIG_HOME/isotone/devices\n"
        "  --channels <n>         channels of the virtual sink: 1, 2, 3, 4, 6, 8 (default\n"
        "                         daemon.conf's, else 2)\n"
        "  --max-frames <n>       frames the processor is sized for (default 8192)\n"
        "  --keep-region          leave the shared region's name behind on exit\n"
        "  --leave-streams        do not move applications' playback into the virtual\n"
        "                         sink; only what plays into it is processed\n"
        "  --exit-when-linked     process a few blocks, then exit (for measurement)\n");
}

bool value_for(int argc, char** argv, int* i, const char* flag, std::string* out) {
    if (std::strcmp(argv[*i], flag) != 0) return false;
    if (*i + 1 >= argc) {
        std::fprintf(stderr, "isotone-daemon: %s needs a value\n", flag);
        std::exit(2);
    }
    *out = argv[++(*i)];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    isotone::daemon::Options options;
    std::string max_frames, channels;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        if (value_for(argc, argv, &i, "--sink", &options.target_sink)) continue;
        if (value_for(argc, argv, &i, "--name", &options.sink_name)) continue;
        if (value_for(argc, argv, &i, "--description", &options.sink_description)) continue;
        if (value_for(argc, argv, &i, "--state-dir", &options.state_dir)) continue;
        if (value_for(argc, argv, &i, "--max-frames", &max_frames)) continue;
        if (value_for(argc, argv, &i, "--channels", &channels)) continue;
        if (std::strcmp(argv[i], "--keep-region") == 0) {
            options.unlink_on_exit = false;
            continue;
        }
        if (std::strcmp(argv[i], "--exit-when-linked") == 0) {
            options.exit_when_linked = true;
            continue;
        }
        if (std::strcmp(argv[i], "--leave-streams") == 0) {
            options.capture_streams = false;
            continue;
        }
        std::fprintf(stderr, "isotone-daemon: unknown argument %s\n", argv[i]);
        return 2;
    }

    if (!channels.empty()) {
        options.channels = static_cast<uint32_t>(std::strtoul(channels.c_str(), nullptr, 10));
        if (!isotone::posix::daemon_channels_supported(options.channels)) {
            std::fprintf(stderr, "isotone-daemon: --channels must be 1, 2, 3, 4, 6 or 8\n");
            return 2;
        }
    } else if (const uint32_t chosen = isotone::posix::read_daemon_channels(isotone::posix::daemon_config_path())) {
        // The layout the UI's picker chose, when no flag overrides it.
        options.channels = chosen;
    }
    if (!max_frames.empty()) {
        options.max_frames = static_cast<uint32_t>(std::strtoul(max_frames.c_str(), nullptr, 10));
        if (options.max_frames == 0) {
            std::fprintf(stderr, "isotone-daemon: --max-frames must be positive\n");
            return 2;
        }
    }

    return isotone::daemon::run(options);
}
