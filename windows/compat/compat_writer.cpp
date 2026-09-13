// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "compat_writer.h"

#include "config_files.h"

namespace isotone::compat {

CompatWriter::CompatWriter(std::filesystem::path config_dir, WriteCoalescer::Clock clock)
    : dir_(std::move(config_dir)),
      path_(dir_ / kIsotoneFileName),
      coalescer_([this](const std::string& bytes) { return write_file_atomically(path_, bytes); },
                 kCompatWriteInterval, std::move(clock)) {}

DWORD CompatWriter::load() {
    const DWORD attrs = GetFileAttributesW(dir_.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;

    std::string bytes;
    const DWORD e = read_file_bytes(path_, &bytes);
    if (e == ERROR_FILE_NOT_FOUND) {
        text_.clear();
        return ERROR_SUCCESS;
    }
    if (e == ERROR_SUCCESS) text_ = std::move(bytes);
    return e;
}

DWORD CompatWriter::apply(const DeviceConfig& device) {
    text_ = update_isotone_file(text_, device);
    return coalescer_.submit(text_);
}

DWORD CompatWriter::persist(const DeviceConfig& device) {
    text_ = update_isotone_file(text_, device);
    const DWORD e = coalescer_.submit(text_);
    return e != ERROR_SUCCESS ? e : coalescer_.flush();
}

DWORD CompatWriter::remove(const std::string& endpoint_guid) {
    text_ = remove_device(text_, endpoint_guid);
    const DWORD e = coalescer_.submit(text_);
    return e != ERROR_SUCCESS ? e : coalescer_.flush();
}

}  // namespace isotone::compat
