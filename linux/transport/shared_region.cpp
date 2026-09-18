// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "shared_region.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace isotone::posix {

namespace {

// A shm name is a slash and then at most NAME_MAX bytes, with no other slash.
constexpr size_t kNameMax = 255;
constexpr char   kPrefix[] = "isotone.";

uint32_t fnv1a(const std::string& text) {
    uint32_t hash = 2166136261u;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

bool is_safe(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

std::string sanitize_key(const std::string& node_name) {
    if (node_name.empty()) return {};

    std::string key;
    key.reserve(node_name.size());
    for (const char c : node_name) key.push_back(is_safe(c) ? c : '_');

    // "/" + prefix + key must fit, and a truncated key keeps a hash of the whole
    // name so two long names stay distinct.
    const size_t room = kNameMax - (sizeof(kPrefix) - 1);
    if (key.size() > room) {
        char suffix[10] = {};
        std::snprintf(suffix, sizeof(suffix), "-%08x", fnv1a(node_name));
        key.resize(room - (sizeof(suffix) - 1));
        key += suffix;
    }
    return key;
}

std::string region_name(const std::string& node_name) {
    const std::string key = sanitize_key(node_name);
    return key.empty() ? std::string() : "/" + std::string(kPrefix) + key;
}

int SharedRegion::map_fd(int fd) {
    void* base = ::mmap(nullptr, kSharedRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return errno;
    base_ = base;
    return 0;
}

int SharedRegion::create_or_open(const std::string& name, void (*seed)(ParamBlock* block, void* context),
                                 void* context) {
    close();
    if (name.empty()) return EINVAL;

    // Create, or open what is already there. Both can lose: another daemon can
    // create between the two calls (EEXIST), and the outgoing one can unlink in
    // the same window (ENOENT), which is exactly what `systemctl --user restart`
    // does. Either way the other outcome is now available, so try again rather
    // than fail the start.
    bool fresh = false;
    int  fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
        fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            fresh = true;
            break;
        }
        if (errno != EEXIST) return errno;
        fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0 && errno != ENOENT) return errno;
    }
    if (fd < 0) return errno;

    // A region left short by a host that died between shm_open and ftruncate
    // would raise SIGBUS on the first touch past its end, not return an error,
    // so the size is settled before anything is mapped.
    struct stat st {};
    if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < kSharedRegionBytes) {
        if (::ftruncate(fd, static_cast<off_t>(kSharedRegionBytes)) != 0) {
            const int error = errno;
            ::close(fd);
            if (fresh) ::shm_unlink(name.c_str());
            return error;
        }
    }

    const int map_error = map_fd(fd);
    ::close(fd);  // the mapping keeps the object alive
    if (map_error != 0) {
        if (fresh) ::shm_unlink(name.c_str());
        return map_error;
    }

    if (fresh) {
        // shm_open zero-fills, so only the headers need writing.
        init_shared_region(base_, seed, context);
        created_ = true;
        return 0;
    }

    // Adopting. The creator writes the magic last, so a zero magic means "not
    // finished yet" rather than "wrong layout"; it finishes in microseconds.
    const int64_t deadline = now_ms() + 50;
    while (params()->hdr.magic == 0 && now_ms() < deadline) {
        timespec nap{0, 1000000};
        ::nanosleep(&nap, nullptr);
    }
    if (!shared_region_valid(base_, kSharedRegionBytes)) {
        init_shared_region(base_, seed, context);
        created_ = true;
    }
    return 0;
}

int SharedRegion::open(const std::string& name) {
    close();
    if (name.empty()) return EINVAL;

    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) return errno;

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int error = errno;
        ::close(fd);
        return error;
    }
    if (static_cast<size_t>(st.st_size) < kSharedRegionBytes) {
        ::close(fd);
        return ENODATA;   // the host has not finished creating it
    }

    const int map_error = map_fd(fd);
    ::close(fd);
    if (map_error != 0) return map_error;

    const int64_t deadline = now_ms() + 50;
    while (params()->hdr.magic == 0 && now_ms() < deadline) {
        timespec nap{0, 1000000};
        ::nanosleep(&nap, nullptr);
    }
    if (!shared_region_valid(base_, kSharedRegionBytes)) {
        close();
        return ENODATA;
    }
    return 0;
}

void SharedRegion::close() {
    if (base_ != nullptr) {
        ::munmap(base_, kSharedRegionBytes);
        base_ = nullptr;
    }
    created_ = false;
}

int SharedRegion::unlink_region(const std::string& name) {
    if (name.empty()) return EINVAL;
    return ::shm_unlink(name.c_str()) == 0 ? 0 : errno;
}

}  // namespace isotone::posix
