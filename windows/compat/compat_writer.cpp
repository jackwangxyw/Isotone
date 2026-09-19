// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "compat_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "config_files.h"

namespace isotone::compat {

namespace {

// As isotone_file.cpp keys devices: braces, whitespace and case do not count.
std::string device_key(const std::string& id) {
    const size_t open = id.rfind('{');
    std::string k;
    for (char c : open == std::string::npos ? id : id.substr(open)) {
        if (c == '{' || c == '}' || std::isspace(static_cast<unsigned char>(c))) continue;
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return k;
}

// Longer than one write holds the lock, which includes up to 200 ms of
// write_file_atomically waiting out a reader.
constexpr DWORD kLockWaitMs = 1000;

}  // namespace

DirectoryLock::~DirectoryLock() {
    if (h_ == INVALID_HANDLE_VALUE) return;
    if (locked_) {
        OVERLAPPED ov{};
        UnlockFileEx(h_, 0, 1, 0, &ov);
    }
    CloseHandle(h_);
}

DWORD DirectoryLock::acquire(const std::filesystem::path& path, DWORD wait_ms) {
    h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) return GetLastError();
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return GetLastError();
    DWORD e = ERROR_SUCCESS;
    if (!LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
        e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, wait_ms) != WAIT_OBJECT_0) CancelIoEx(h_, &ov);
            // Granted as the wait ran out counts as granted.
            DWORD unused = 0;
            e = GetOverlappedResult(h_, &ov, &unused, TRUE) ? ERROR_SUCCESS : GetLastError();
            if (e == ERROR_OPERATION_ABORTED) e = ERROR_SHARING_VIOLATION;
        }
    }
    CloseHandle(ov.hEvent);
    locked_ = e == ERROR_SUCCESS;
    return e;
}

CompatWriter::CompatWriter(std::filesystem::path config_dir)
    : dir_(std::move(config_dir)), path_(dir_ / kIsotoneFileName) {}

CompatWriter::~CompatWriter() {
    flush();
}

DWORD CompatWriter::flush() {
    return pending_ ? write() : ERROR_SUCCESS;
}

DWORD CompatWriter::write() {
    pending_ = true;
    std::filesystem::path lock_path = path_;
    lock_path += L".lock";
    DirectoryLock lock;
    if (const DWORD e = lock.acquire(lock_path, kLockWaitMs); e != ERROR_SUCCESS) return e;

    std::string current;
    const DWORD e = read_file_bytes(path_, &current);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return e;
    std::string out = text_;
    if (current != on_disk_) {
        // Another writer changed the file: keep its blocks, and put this
        // writer's devices back on top.
        out = current;
        for (const auto& [key, device] : mine_) {
            out = device ? update_isotone_file(out, *device) : remove_device(out, key);
        }
        text_ = out;
    }
    // What is on disk decides whether to write, not what this writer last
    // wrote: another writer, or a deleted file, may have changed it since.
    if (e == ERROR_SUCCESS && out == current) {
        on_disk_ = current;
        pending_ = false;
        return ERROR_SUCCESS;
    }
    const DWORD w = write_file_atomically(path_, out);
    if (w == ERROR_SUCCESS) {
        on_disk_ = out;
        pending_ = false;
    }
    return w;
}

DWORD CompatWriter::load() {
    const DWORD attrs = GetFileAttributesW(dir_.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;

    std::string bytes;
    const DWORD e = read_file_bytes(path_, &bytes);
    if (e == ERROR_FILE_NOT_FOUND) {
        text_.clear();
        on_disk_.clear();
        return ERROR_SUCCESS;
    }
    if (e == ERROR_SUCCESS) {
        text_ = std::move(bytes);
        on_disk_ = text_;
    }
    return e;
}

namespace {

void remember(std::vector<std::pair<std::string, std::optional<DeviceConfig>>>* mine, const std::string& key,
              std::optional<DeviceConfig> device) {
    const auto it = std::find_if(mine->begin(), mine->end(), [&](const auto& m) { return m.first == key; });
    if (it != mine->end()) mine->erase(it);
    mine->emplace_back(key, std::move(device));
}

// Routing is written for a channel count and bands for a rate, and a guess at
// either is wrong on the device: there is no safe default. No stream has more
// than kMaxApoChannels channels.
bool has_format(const DeviceConfig& device) {
    return device.layout.channels != 0 && device.layout.channels <= kMaxApoChannels && device.sample_rate > 0.0 &&
           std::isfinite(device.sample_rate);
}

}  // namespace

DWORD CompatWriter::apply(const DeviceConfig& device) {
    if (!has_format(device)) return ERROR_INVALID_PARAMETER;
    remember(&mine_, device_key(device.endpoint_guid), device);
    text_ = update_isotone_file(text_, device);
    pending_ = true;
    return ERROR_SUCCESS;
}

DWORD CompatWriter::persist(const DeviceConfig& device) {
    if (!has_format(device)) return ERROR_INVALID_PARAMETER;
    remember(&mine_, device_key(device.endpoint_guid), device);
    text_ = update_isotone_file(text_, device);
    return write();
}

DWORD CompatWriter::remove(const std::string& endpoint_guid) {
    remember(&mine_, device_key(endpoint_guid), std::nullopt);
    text_ = remove_device(text_, endpoint_guid);
    return write();
}

}  // namespace isotone::compat
