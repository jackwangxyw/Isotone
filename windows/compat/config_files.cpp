// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "config_files.h"

#include <algorithm>
#include <cctype>

#include "eapo_install.h"

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

// True when an Include value names the Isotone.txt next to config.txt, which is
// the file this backend writes. Upstream resolves a relative include against the
// including file's directory, so "Isotone.txt", ".\Isotone.txt" and an absolute
// path to the same directory are all the same include.
bool names_isotone_file(const std::string& value, const fs::path& config_dir) {
    const std::string v = trim(value);
    const int n = MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), wide.data(), n);
    fs::path p = wide;
    if (_wcsicmp(p.filename().c_str(), L"Isotone.txt") != 0) return false;
    if (p.is_relative()) p = config_dir / p;
    const fs::path dir = p.parent_path().lexically_normal();
    return dir.lexically_normal() == config_dir.lexically_normal() || same_file_object(dir, config_dir);
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

    // CREATE_ALWAYS on an existing name writes through it, so a .tmp planted as
    // a hard link to another file would be overwritten. Remove the name, which
    // only unlinks it, and create a new file.
    DeleteFileW(tmp.c_str());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
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

namespace {

// Opens config.txt to check and change it through one handle. Other writers are
// shut out while it is held; Equalizer APO, which opens with read sharing only,
// waits (FilterEngine::loadConfigFile retries a sharing violation), and so does
// this, for up to `retry_ms`. A file that is a hard link or a reparse point is
// refused: writing it would change a file somewhere else.
DWORD open_for_update(const fs::path& path, HANDLE* out, DWORD retry_ms = 1000) {
    const ULONGLONG deadline = GetTickCount64() + retry_ms;
    for (;;) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info)) {
                const DWORD e = GetLastError();
                CloseHandle(h);
                return e;
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nNumberOfLinks > 1) {
                CloseHandle(h);
                return ERROR_CANT_ACCESS_FILE;
            }
            *out = h;
            return ERROR_SUCCESS;
        }
        const DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION || GetTickCount64() >= deadline) return e;
        Sleep(1);
    }
}

DWORD read_handle(HANDLE h, std::string* out) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) return GetLastError();
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return GetLastError();
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1 << 20));
        if (!ReadFile(h, bytes.data() + done, want, &got, nullptr)) return GetLastError();
        if (got == 0) break;
        done += got;
    }
    bytes.resize(done);
    *out = std::move(bytes);
    return ERROR_SUCCESS;
}

}  // namespace

ConfigInspection inspect_config(const fs::path& config_dir) {
    ConfigInspection r;
    std::string bytes;
    r.error = read_file_bytes(config_dir / "config.txt", &bytes);
    if (r.error != ERROR_SUCCESS) return r;
    return inspect_config_text(bytes, config_dir);
}

ConfigInspection inspect_config_text(const std::string& bytes, const fs::path& config_dir) {
    ConfigInspection r;
    // Upstream skips what follows a Device line that does not match the device,
    // and what is inside an If that is false. Which devices match, and which
    // conditions hold, only the device can say; `Device: all` and no If reach
    // every device.
    bool every_device = true;
    int if_depth = 0;
    for (const std::string& line : config_lines(bytes)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = line.substr(colon + 1);
        if (key == "Include") {
            r.includes.push_back(trim(value));
            if (names_isotone_file(value, config_dir)) {
                (every_device && if_depth == 0 ? r.isotone_included : r.isotone_included_conditionally) = true;
            }
            r.peace_included |= names_peace_file(value);
        } else if (key == "Device") {
            std::string pattern = trim(value);
            for (char& c : pattern) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            every_device = pattern == "all";
        } else if (key == "Stage") {
            r.has_stage_lines = true;
        } else if (key == "If" || key == "ElseIf" || key == "Else" || key == "EndIf") {
            r.has_conditionals = true;
            if (key == "If") ++if_depth;
            if (key == "EndIf" && if_depth > 0) --if_depth;
        }
    }
    r.attached_by_isotone =
        ends_with(bytes, attach_block("\r\n")) || ends_with(bytes, attach_block("\n"));
    return r;
}

AttachResult attach_include(const fs::path& config_dir) {
    AttachResult r;
    const fs::path config = config_dir / "config.txt";

    // The backup is taken first, from an ordinary read: writing it while
    // config.txt is held would make Equalizer APO's reload wait on this.
    std::string snapshot;
    if ((r.error = read_file_bytes(config, &snapshot)) != ERROR_SUCCESS) {
        r.before.error = r.error;
        return r;
    }
    r.before = inspect_config_text(snapshot, config_dir);
    if (r.before.isotone_included) {
        return r;
    }
    if (r.before.isotone_included_conditionally) {
        r.error = ERROR_ALREADY_EXISTS;
        return r;
    }
    r.backup = config_dir / kConfigBackupName;
    if ((r.error = write_file_atomically(r.backup, snapshot)) != ERROR_SUCCESS) return r;

    // Check and append through one handle, so nothing can change the file in
    // between. Append only: every existing byte, and the file's security
    // descriptor, stay.
    HANDLE h = INVALID_HANDLE_VALUE;
    if ((r.error = open_for_update(config, &h)) != ERROR_SUCCESS) return r;
    std::string original;
    r.error = read_handle(h, &original);
    if (r.error == ERROR_SUCCESS && original != snapshot) {
        // Changed after the backup was taken: append nothing, report it.
        r.error = ERROR_INVALID_DATA;
    }
    if (r.error != ERROR_SUCCESS) {
        CloseHandle(h);
        return r;
    }
    const std::string block = attach_block(newline_of(original));
    LARGE_INTEGER zero{};
    DWORD written = 0;
    const bool ok = SetFilePointerEx(h, zero, nullptr, FILE_END) &&
                    WriteFile(h, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) &&
                    written == block.size() && FlushFileBuffers(h);
    r.error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    r.appended = ok;
    return r;
}

DWORD detach_include(const fs::path& config_dir, bool* removed) {
    *removed = false;
    const fs::path config = config_dir / "config.txt";

    // Check and truncate through one handle, so the cut is made in the file
    // that was checked.
    HANDLE h = INVALID_HANDLE_VALUE;
    if (const DWORD e = open_for_update(config, &h); e != ERROR_SUCCESS) return e;
    std::string bytes;
    if (const DWORD e = read_handle(h, &bytes); e != ERROR_SUCCESS) {
        CloseHandle(h);
        return e;
    }

    std::string block;
    for (const char* nl : {"\r\n", "\n"}) {
        if (ends_with(bytes, attach_block(nl))) block = attach_block(nl);
    }
    if (block.empty()) {
        CloseHandle(h);
        return inspect_config_text(bytes, config_dir).isotone_included ? ERROR_INVALID_DATA : ERROR_SUCCESS;
    }

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
