// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "config_attach.h"

#include <algorithm>
#include <cctype>

#include "config_files.h"

namespace isotone::ui {

namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

// config_files.cpp's reading of a line: the key before the first ':', trimmed
// and case-sensitive; a value naming peace.txt, by file name in any case.
bool is_peace_include(const std::string& line) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos || trim(line.substr(0, colon)) != "Include") return false;
    std::string value = trim(line.substr(colon + 1));
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t slash = value.find_last_of("\\/");
    return (slash == std::string::npos ? value : value.substr(slash + 1)) == "peace.txt";
}

// Upstream splits on '\n' and drops one trailing '\r'.
std::vector<std::string> split_lines(const std::string& bytes) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < bytes.size()) {
        const size_t nl = bytes.find('\n', start);
        std::string line = bytes.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

}  // namespace

std::string without_peace_includes(const std::string& bytes) {
    std::string out;
    size_t start = 0;
    while (start < bytes.size()) {
        const size_t nl = bytes.find('\n', start);
        const size_t end = nl == std::string::npos ? bytes.size() : nl + 1;
        if (!is_peace_include(bytes.substr(start, end - start))) out += bytes.substr(start, end - start);
        start = end;
    }
    return out;
}

AttachPreview preview_attach(const fs::path& config_dir) {
    AttachPreview p;
    std::string bytes;
    if ((p.error = isotone::compat::read_file_bytes(config_dir / "config.txt", &bytes)) != ERROR_SUCCESS) return p;
    p.lines = split_lines(bytes);
    for (const std::string& line : p.lines) p.peace.push_back(is_peace_include(line));
    p.attached = isotone::compat::inspect_config_text(bytes, config_dir).isotone_included;
    if (p.attached) return p;

    // attach_include on a copy: its block is what it appends to these bytes.
    std::error_code ec;
    const fs::path scratch = fs::temp_directory_path(ec) /
                             ("isotone-attach-preview-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()));
    if (ec || !fs::create_directories(scratch, ec)) {
        p.error = ec ? static_cast<DWORD>(ec.value()) : ERROR_ALREADY_EXISTS;
        return p;
    }
    DWORD error = isotone::compat::write_file_atomically(scratch / "config.txt", bytes);
    if (error == ERROR_SUCCESS) error = isotone::compat::attach_include(scratch).error;
    std::string attached;
    if (error == ERROR_SUCCESS) error = isotone::compat::read_file_bytes(scratch / "config.txt", &attached);
    fs::remove_all(scratch, ec);
    if (error != ERROR_SUCCESS || attached.size() < bytes.size()) {
        p.error = error != ERROR_SUCCESS ? error : ERROR_INVALID_DATA;
        return p;
    }
    // After a last line with no break, the block starts with one.
    std::string block = attached.substr(bytes.size());
    if (!block.empty() && block.front() == '\r') block.erase(0, 1);
    if (!block.empty() && block.front() == '\n') block.erase(0, 1);
    p.added = split_lines(block);
    return p;
}

AttachOutcome attach_config(const fs::path& config_dir, bool remove_peace) {
    AttachOutcome o;
    if (remove_peace) {
        const fs::path config = config_dir / "config.txt";
        std::string bytes;
        if ((o.error = isotone::compat::read_file_bytes(config, &bytes)) != ERROR_SUCCESS) return o;
        const std::string kept = without_peace_includes(bytes);
        if (kept != bytes) {
            if ((o.error = isotone::compat::write_file_atomically(config, kept)) != ERROR_SUCCESS) return o;
            o.peace_removed = true;
        }
    }
    const isotone::compat::AttachResult r = isotone::compat::attach_include(config_dir);
    o.error = r.error;
    o.appended = r.appended;
    return o;
}

}  // namespace isotone::ui
