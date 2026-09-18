// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "persisted_state.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "shared_region.h"

namespace isotone::posix {

namespace {

// Only the parameters travel: the header's seqlock and host fields describe a
// live region.
ParamBlock parameters_only(const ParamBlock& block) {
    ParamBlock copy = block;
    copy.hdr.seq = 0;
    copy.hdr.sample_rate = 0;
    copy.hdr.channels = 0;
    copy.hdr.host_state = 0;
    copy.hdr.host_heartbeat = 0;
    copy.hdr.speaker_mask = 0;
    for (uint32_t& r : copy.hdr.host_reserved) r = 0;
    return copy;
}

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

int fsync_path(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return errno;
    const int error = ::fsync(fd) == 0 ? 0 : errno;
    ::close(fd);
    return error;
}

bool read_exactly(int fd, void* out, size_t bytes) {
    auto* p = static_cast<unsigned char*>(out);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::read(fd, p + done, bytes - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool write_exactly(int fd, const void* in, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(in);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::write(fd, p + done, bytes - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

std::string persisted_state_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    // The specification says a relative XDG_CONFIG_HOME is invalid and must be
    // ignored, not resolved against the working directory.
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/isotone/devices";

    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] == '/') return std::string(home) + "/.config/isotone/devices";
    return {};
}

std::string persisted_state_path(const std::string& dir, const std::string& node_name) {
    const std::string key = sanitize_key(node_name);
    return key.empty() || dir.empty() ? std::string() : dir + "/" + key + ".bin";
}

PersistedRead read_persisted_state(const std::string& path, ParamBlock* out) {
    if (path.empty() || out == nullptr) return PersistedRead::Invalid;

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? PersistedRead::Absent : PersistedRead::Invalid;

    struct stat st {};
    ParamBlock block{};
    const bool ok = ::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) == sizeof(ParamBlock) &&
                    read_exactly(fd, &block, sizeof(block));
    ::close(fd);

    if (!ok || !param_block_valid(block)) return PersistedRead::Invalid;
    *out = parameters_only(block);
    return PersistedRead::Loaded;
}

int write_persisted_state(const std::string& path, const ParamBlock& block) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return EINVAL;
    const std::string dir = path.substr(0, slash);
    if (const int error = make_directories(dir); error != 0) return error;

    // The header is this build's whatever the caller's block holds: a block
    // filled by to_param_block alone carries none, and would read back Invalid.
    ParamBlock copy = parameters_only(block);
    copy.hdr.magic = kParamMagic;
    copy.hdr.version = kParamVersion;
    copy.hdr.size = sizeof(ParamBlock);

    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    bool ok = write_exactly(fd, &copy, sizeof(copy));
    if (ok) ok = ::fsync(fd) == 0;
    // A short write leaves errno untouched, and it may be 0: reporting that
    // would be success with no file written.
    int write_error = 0;
    if (!ok) write_error = errno != 0 ? errno : EIO;
    ::close(fd);
    if (!ok) {
        ::unlink(temp.c_str());
        return write_error;
    }

    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return error;
    }
    // The directory's fsync is what keeps the rename itself from being lost, but
    // the state is already written, fsynced and in place by now. Some
    // filesystems refuse fsync on a directory, and failing the call there would
    // report a save that did happen as a failure.
    if (const int error = fsync_path(dir, O_RDONLY | O_DIRECTORY); error != 0) {
        std::fprintf(stderr, "isotone: %s saved, but its directory could not be synced: %s" "\n",
                     path.c_str(), std::strerror(error));
    }
    return 0;
}

}  // namespace isotone::posix
