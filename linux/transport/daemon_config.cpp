// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "daemon_config.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace isotone::posix {

namespace {

constexpr const char* kChannelsKey = "channels=";

}  // namespace

bool daemon_channels_supported(uint32_t channels) {
    return channels == 1 || channels == 2 || channels == 3 || channels == 4 || channels == 6 || channels == 8;
}

std::string daemon_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    // A relative XDG_CONFIG_HOME is invalid and ignored, as persisted_state_dir does.
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/isotone/daemon.conf";
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') return std::string(home) + "/.config/isotone/daemon.conf";
    return {};
}

uint32_t read_daemon_channels(const std::string& path) {
    if (path.empty()) return 0;
    std::ifstream in(path);
    std::string line;
    uint32_t channels = 0;
    while (std::getline(in, line)) {
        if (line.rfind(kChannelsKey, 0) != 0) continue;
        char* end = nullptr;
        const std::string value = line.substr(std::char_traits<char>::length(kChannelsKey));
        const unsigned long n = std::strtoul(value.c_str(), &end, 10);
        channels = end != value.c_str() && *end == '\0' ? static_cast<uint32_t>(n) : 0;
    }
    return daemon_channels_supported(channels) ? channels : 0;
}

int write_daemon_channels(const std::string& path, uint32_t channels) {
    if (path.empty() || !daemon_channels_supported(channels)) return EINVAL;

    std::vector<std::string> kept;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
            if (line.rfind(kChannelsKey, 0) != 0) kept.push_back(line);
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) return ec.value();

    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        for (const std::string& line : kept) out << line << "\n";
        out << kChannelsKey << channels << "\n";
        out.flush();
        if (!out) return EIO;
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return error;
    }
    return 0;
}

}  // namespace isotone::posix
