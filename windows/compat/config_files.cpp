// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "config_files.h"

#include <algorithm>
#include <cctype>

namespace isotone::compat {
namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Upstream splits on '\n' and drops one trailing '\r' (FilterEngine::loadConfigFile).
std::vector<std::string> config_lines(const std::string& bytes) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= bytes.size()) {
        size_t nl = bytes.find('\n', start);
        std::string line = bytes.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

// A relative include of Isotone.txt resolves next to config.txt, which is the
// file this backend writes.
bool names_isotone_file(const std::string& value) {
    std::string v = lower(trim(value));
    if (v.rfind(".\\", 0) == 0 || v.rfind("./", 0) == 0) v = v.substr(2);
    return v == lower(kIsotoneFileName);
}

bool names_peace_file(const std::string& value) {
    const std::string v = lower(trim(value));
    const size_t slash = v.find_last_of("\\/");
    return (slash == std::string::npos ? v : v.substr(slash + 1)) == "peace.txt";
}

std::string newline_of(const std::string& bytes) {
    if (bytes.find("\r\n") != std::string::npos) return "\r\n";
    if (bytes.find('\n') != std::string::npos) return "\n";
    return "\r\n";
}

std::string attach_block(const std::string& nl) {
    return nl + "# Added by Isotone. Remove these three lines to detach it." + nl +
           "Device: all" + nl + "Include: " + kIsotoneFileName + nl;
}

DWORD open_error(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return ERROR_DIRECTORY;
    return ERROR_SUCCESS;
}

}  // namespace

DWORD read_file_bytes(const fs::path& path, std::string* out) {
    if (const DWORD e = open_error(path); e != ERROR_SUCCESS) return e;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        return e;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1 << 20));
        if (!ReadFile(h, bytes.data() + done, want, &got, nullptr)) {
            const DWORD e = GetLastError();
            CloseHandle(h);
            return e;
        }
        if (got == 0) break;
        done += got;
    }
    CloseHandle(h);
    bytes.resize(done);
    *out = std::move(bytes);
    return ERROR_SUCCESS;
}

DWORD write_file_atomically(const fs::path& path, const std::string& bytes, DWORD retry_ms) {
    fs::path tmp = path;
    tmp += L".tmp";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                     written == bytes.size());
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
    const bool flushed = ok && FlushFileBuffers(h);
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok || !flushed) {
        DeleteFileW(tmp.c_str());
        return !ok ? (write_error != ERROR_SUCCESS ? write_error : ERROR_WRITE_FAULT) : flush_error;
    }

    const ULONGLONG deadline = GetTickCount64() + retry_ms;
    for (;;) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return ERROR_SUCCESS;
        }
        const DWORD e = GetLastError();
        const bool contended = e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ||
                               e == ERROR_LOCK_VIOLATION;
        if (!contended || GetTickCount64() >= deadline) {
            DeleteFileW(tmp.c_str());
            return e;
        }
        Sleep(1);
    }
}

ConfigInspection inspect_config(const fs::path& config_dir) {
    ConfigInspection r;
    std::string bytes;
    r.error = read_file_bytes(config_dir / "config.txt", &bytes);
    if (r.error != ERROR_SUCCESS) return r;

    for (const std::string& line : config_lines(bytes)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = line.substr(colon + 1);
        if (key == "Include") {
            r.includes.push_back(trim(value));
            r.isotone_included |= names_isotone_file(value);
            r.peace_included |= names_peace_file(value);
        } else if (key == "Stage") {
            r.has_stage_lines = true;
        } else if (key == "If" || key == "ElseIf" || key == "Else" || key == "EndIf") {
            r.has_conditionals = true;
        }
    }
    r.attached_by_isotone =
        ends_with(bytes, attach_block("\r\n")) || ends_with(bytes, attach_block("\n"));
    return r;
}

AttachResult attach_include(const fs::path& config_dir) {
    AttachResult r;
    r.before = inspect_config(config_dir);
    if (r.before.error != ERROR_SUCCESS) {
        r.error = r.before.error;
        return r;
    }
    if (r.before.isotone_included) {
        return r;
    }

    const fs::path config = config_dir / "config.txt";
    std::string original;
    if ((r.error = read_file_bytes(config, &original)) != ERROR_SUCCESS) return r;
    const std::string block = attach_block(newline_of(original));

    r.backup = config_dir / kConfigBackupName;
    if ((r.error = write_file_atomically(r.backup, original)) != ERROR_SUCCESS) return r;

    // Append only: every existing byte, and the file's security descriptor, stay.
    HANDLE h = CreateFileW(config.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.error = GetLastError();
        return r;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) &&
                    written == block.size() && FlushFileBuffers(h);
    r.error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok) return r;

    // Prove it: the file must be exactly what was read plus the block. Anything
    // else means someone else wrote to config.txt at the same moment.
    std::string after;
    if ((r.error = read_file_bytes(config, &after)) != ERROR_SUCCESS) return r;
    if (after != original + block) {
        r.error = ERROR_INVALID_DATA;
        return r;
    }
    r.appended = true;
    return r;
}

DWORD detach_include(const fs::path& config_dir, bool* removed) {
    *removed = false;
    const fs::path config = config_dir / "config.txt";
    std::string bytes;
    if (const DWORD e = read_file_bytes(config, &bytes); e != ERROR_SUCCESS) return e;

    std::string block;
    for (const char* nl : {"\r\n", "\n"}) {
        if (ends_with(bytes, attach_block(nl))) block = attach_block(nl);
    }
    if (block.empty()) {
        const ConfigInspection i = inspect_config(config_dir);
        return i.isotone_included ? ERROR_INVALID_DATA : ERROR_SUCCESS;
    }

    HANDLE h = CreateFileW(config.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(bytes.size() - block.size());
    const bool ok = SetFilePointerEx(h, end, nullptr, FILE_BEGIN) && SetEndOfFile(h) &&
                    FlushFileBuffers(h);
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok) return e;
    *removed = true;
    return ERROR_SUCCESS;
}

}  // namespace isotone::compat
