// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "autostart_xdg.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace isotone::ui {

namespace {

// mkdir -p. Returns an errno value.
int make_directories(const std::string& dir) {
    if (dir.empty()) return EINVAL;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return errno;
    }
    return 0;
}

std::string trimmed(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// The value of `key` in a desktop entry, or empty. Good enough for a file this
// writes itself: no group handling, because there is only ever one group here.
std::string value_of(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (trimmed(line.substr(0, equals)) != key) continue;
        return trimmed(line.substr(equals + 1));
    }
    return {};
}

}  // namespace

std::string autostart_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    // A relative XDG_CONFIG_HOME is invalid and is ignored, not resolved against
    // the working directory.
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/autostart";

    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') return std::string(home) + "/.config/autostart";
    return {};
}

std::string autostart_path(const std::string& dir) {
    return dir.empty() ? std::string() : dir + "/" + kAutostartFileName;
}

namespace {

// The program as one Exec argument (the desktop entry specification, "The Exec
// key"): quoted whole when it holds a reserved character, with the quote, the
// backtick, the dollar and the backslash itself backslashed inside the quotes,
// and that backslash doubled by the string escape rule, which is applied first.
// A % is doubled either way, or it is a field code.
std::string exec_argument(const std::string& program) {
    const bool reserved = program.find_first_of(" \t\"'\\><~|&;$*?#()`") != std::string::npos;
    std::string out;
    if (reserved) out += '"';
    for (const char c : program) {
        if (c == '%') {
            out += "%%";
        } else if (reserved && (c == '"' || c == '`' || c == '$' || c == '\\')) {
            out += "\\\\";
            out += c == '\\' ? "\\\\" : std::string(1, c);
        } else {
            out += c;
        }
    }
    if (reserved) out += '"';
    return out;
}

}  // namespace

std::string autostart_contents(const std::string& exec, bool tray) {
    std::ostringstream out;
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Isotone\n"
        << "Comment=System-wide equalizer\n"
        << "Exec=" << exec_argument(exec) << (tray ? " --tray" : "") << "\n"
        << "Terminal=false\n"
        // Every desktop this targets reads the directory, but each also honours
        // OnlyShowIn/NotShowIn, so neither is written: the entry is for all of
        // them.
        << "X-GNOME-Autostart-enabled=true\n";
    return out.str();
}

int write_autostart(const std::string& path, const std::string& exec, bool tray) {
    if (path.empty()) return EINVAL;
    // A desktop entry value ends at the newline, so one here would write a
    // second key rather than a longer command.
    if (exec.find('\n') != std::string::npos || exec.find('\r') != std::string::npos) return EINVAL;

    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return EINVAL;
    if (const int error = make_directories(path.substr(0, slash)); error != 0) return error;

    // Written whole and renamed, so a desktop reading the directory at the wrong
    // moment never sees half an entry.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return errno != 0 ? errno : EIO;
        out << autostart_contents(exec, tray);
        out.flush();
        if (!out) {
            ::unlink(temp.c_str());
            return errno != 0 ? errno : EIO;
        }
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return error;
    }
    return 0;
}

int remove_autostart(const std::string& path) {
    if (path.empty()) return EINVAL;
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) return 0;
    return errno;
}

bool autostart_enabled(const std::string& path) {
    if (path.empty()) return false;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    // A desktop that lets a person switch an entry off writes one of these
    // rather than deleting the file.
    if (value_of(path, "Hidden") == "true") return false;
    if (value_of(path, "X-GNOME-Autostart-enabled") == "false") return false;
    return true;
}

std::string autostart_command(const std::string& path) { return value_of(path, "Exec"); }

}  // namespace isotone::ui
