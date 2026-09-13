// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "compat_writer.h"

#include <algorithm>
#include <cctype>

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

}  // namespace

CompatWriter::CompatWriter(std::filesystem::path config_dir, WriteCoalescer::Clock clock)
    : dir_(std::move(config_dir)),
      path_(dir_ / kIsotoneFileName),
      coalescer_([this](const std::string& bytes) { return write(bytes); }, kCompatWriteInterval, std::move(clock)) {}

CompatWriter::~CompatWriter() {
    coalescer_.flush();
}

DWORD CompatWriter::write(const std::string& bytes) {
    std::string current;
    const DWORD e = read_file_bytes(path_, &current);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return e;
    std::string out = bytes;
    if (current != on_disk_) {
        // Another writer changed the file: keep its blocks, and put this
        // writer's devices back on top.
        out = current;
        for (const auto& [key, device] : mine_) {
            out = device ? update_isotone_file(out, *device) : remove_device(out, key);
        }
        text_ = out;
    }
    const DWORD w = write_file_atomically(path_, out);
    if (w == ERROR_SUCCESS) on_disk_ = out;
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

}  // namespace

DWORD CompatWriter::apply(const DeviceConfig& device) {
    remember(&mine_, device_key(device.endpoint_guid), device);
    text_ = update_isotone_file(text_, device);
    return coalescer_.submit(text_);
}

DWORD CompatWriter::persist(const DeviceConfig& device) {
    remember(&mine_, device_key(device.endpoint_guid), device);
    text_ = update_isotone_file(text_, device);
    const DWORD e = coalescer_.submit(text_);
    return e != ERROR_SUCCESS ? e : coalescer_.flush();
}

DWORD CompatWriter::remove(const std::string& endpoint_guid) {
    remember(&mine_, device_key(endpoint_guid), std::nullopt);
    text_ = remove_device(text_, endpoint_guid);
    const DWORD e = coalescer_.submit(text_);
    return e != ERROR_SUCCESS ? e : coalescer_.flush();
}

}  // namespace isotone::compat
