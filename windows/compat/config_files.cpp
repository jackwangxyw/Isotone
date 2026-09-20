// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "config_files.h"

#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

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

constexpr char kAttachComment[] = "# Added by Isotone. Remove these ";
constexpr char kAfterLineBreak[] = " and the line break before them";
constexpr char kStageReset[] = "Stage: post-mix capture";

// An `EndIf:` for each If config.txt leaves open, and `Stage: post-mix capture`
// for each Device pattern whose Stage lines leave the end of the file for other
// instances, each under the Device pattern the If or Stage line was under, since
// upstream skips both on a device that pattern does not match; then
// `Device: all` and the Include. A Device line is written only where the
// pattern changes. After a line break, which the comment counts, when the
// file's last line has none.
std::string attach_block(const std::string& nl, const ConfigInspection& closing, bool after_line_break) {
    static constexpr const char* kWords[] = {"three", "four", "five", "six", "seven", "eight", "nine"};
    std::vector<std::string> lines;
    std::optional<std::string> device;
    const auto under = [&](const std::string& pattern) {
        if (device != pattern) lines.push_back("Device: " + pattern);
        device = pattern;
    };
    for (const auto& [pattern, count] : closing.open_ifs_under) {
        under(pattern);
        lines.insert(lines.end(), count, "EndIf:");
    }
    for (const std::string& pattern : closing.stage_changed_under) {
        under(pattern);
        lines.push_back(kStageReset);
    }
    under("all");
    const size_t count = lines.size() + 2;
    std::string out = (after_line_break ? nl : "") + kAttachComment +
                      (count - 3 < 7 ? kWords[count - 3] : std::to_string(count)) + " lines" +
                      (after_line_break ? kAfterLineBreak : "") + " to detach it." + nl;
    for (const std::string& line : lines) out += line + nl;
    return out + "Include: " + kIsotoneFileName + nl;
}

// The size of the block attach_include appended, when it is the file's tail; 0
// when it is not.
size_t attached_block_size(const std::string& bytes) {
    const size_t at = bytes.rfind(kAttachComment);
    if (at == std::string::npos || (at != 0 && bytes[at - 1] != '\n')) return 0;
    const size_t eol = bytes.find('\n', at);
    if (eol == std::string::npos) return 0;
    const std::string nl = bytes[eol - 1] == '\r' ? "\r\n" : "\n";
    const bool after_line_break = bytes.substr(at, eol - at).find(kAfterLineBreak) != std::string::npos;
    if (after_line_break && (at < nl.size() || bytes.compare(at - nl.size(), nl.size(), nl) != 0)) return 0;

    // What the lines after the comment close, then the block that closes it.
    ConfigInspection closing;
    std::string device;
    for (size_t pos = eol + 1;;) {
        const size_t end = bytes.find(nl, pos);
        if (end == std::string::npos) break;
        const std::string line = bytes.substr(pos, end - pos);
        pos = end + nl.size();
        if (line.rfind("Device: ", 0) == 0) {
            device = line.substr(8);
        } else if (line == "EndIf:") {
            auto& open = closing.open_ifs_under;
            if (!open.empty() && open.back().first == device) {
                ++open.back().second;
            } else {
                open.emplace_back(device, 1);
            }
        } else if (line == kStageReset) {
            closing.stage_changed_under.push_back(device);
        } else {
            break;
        }
    }
    const size_t start = after_line_break ? at - nl.size() : at;
    return bytes.compare(start, std::string::npos, attach_block(nl, closing, after_line_break)) == 0
               ? bytes.size() - start
               : 0;
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

namespace {

// MoveFileExW returns ERROR_ACCESS_DENIED both for a replace that waits on a
// reader and for one that can never succeed. Measured on NTFS: a target held
// with FILE_SHARE_READ (as Equalizer APO opens it) refuses a DELETE open with
// ERROR_SHARING_VIOLATION, and one held with FILE_SHARE_DELETE as well grants
// it, yet both block the replace; a read-only file or a directory grants it and
// can never be replaced; an ACL that denies delete refuses it with
// ERROR_ACCESS_DENIED. So does a file pending deletion, which is treated as
// permanent here: the caller's next attempt sees the name gone.
bool replace_denied_for_good(const fs::path& target) {
    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return true;
    }
    HANDLE h = CreateFileW(target.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }
    const DWORD e = GetLastError();
    return e != ERROR_SHARING_VIOLATION && e != ERROR_FILE_NOT_FOUND;
}

DWORD win32_error(HRESULT hr) {
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : static_cast<DWORD>(hr);
}

// %LOCALAPPDATA%\Isotone\compat-tmp, not created.
DWORD default_temp_dir(fs::path* out) {
    PWSTR local = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local);
    if (SUCCEEDED(hr)) *out = fs::path(local) / L"Isotone" / L"compat-tmp";
    CoTaskMemFree(local);
    return SUCCEEDED(hr) ? ERROR_SUCCESS : win32_error(hr);
}

// The volume GUID path (\\?\Volume{...}\) `path` is on, following junctions and
// mounted folders, for a path that need not exist yet. Empty when there is none:
// a network share, a drive letter nothing is mounted on.
std::wstring volume_guid_path(const fs::path& path) {
    std::vector<wchar_t> mount(path.native().size() + MAX_PATH);
    wchar_t guid[64] = {};
    if (!GetVolumePathNameW(path.c_str(), mount.data(), static_cast<DWORD>(mount.size())) ||
        !GetVolumeNameForVolumeMountPointW(mount.data(), guid, 64)) {
        return {};
    }
    return guid;
}

DWORD read_security(const fs::path& path, SECURITY_INFORMATION what, std::vector<BYTE>* sd) {
    DWORD need = 0;
    if (!GetFileSecurityW(path.c_str(), what, nullptr, 0, &need) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return GetLastError();
    }
    sd->assign(need, 0);
    return GetFileSecurityW(path.c_str(), what, sd->data(), need, &need) ? ERROR_SUCCESS : GetLastError();
}

// The DACL `path` has, or, when there is no file there yet, the one a file
// created there by this process gets: the directory's inheritable ACEs, marked
// inherited when the directory's are (NTFS marks them only then), or the
// token's default DACL when there are none.
DWORD dacl_for(const fs::path& path, std::vector<BYTE>* sd) {
    DWORD e = read_security(path, DACL_SECURITY_INFORMATION, sd);
    // A target another writer has left pending deletion refuses this with
    // ERROR_ACCESS_DENIED, and a moment later is not there at all. Returning
    // that error failed the whole write before it ever reached the replace and
    // its retry loop, which is how two writers racing on one path lost: three
    // threads over 200 rounds each, in CI on 2026-09-20.
    //
    // Waited out rather than treated as missing, because ERROR_ACCESS_DENIED
    // from a DACL read is also what a target genuinely out of reach gives, and
    // that one should still be reported. 50 ms is far longer than a pending
    // deletion lasts and inside every deadline above this.
    if (e == ERROR_ACCESS_DENIED) {
        const ULONGLONG deadline = GetTickCount64() + 50;
        while (e == ERROR_ACCESS_DENIED && GetTickCount64() < deadline) {
            Sleep(0);
            e = read_security(path, DACL_SECURITY_INFORMATION, sd);
        }
    }
    if (e != ERROR_FILE_NOT_FOUND) return e;

    std::vector<BYTE> parent;
    const SECURITY_INFORMATION all = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (const DWORD pe = read_security(path.parent_path(), all, &parent); pe != ERROR_SUCCESS) return pe;
    WORD control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(parent.data(), &control, &revision)) return GetLastError();
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
    GENERIC_MAPPING mapping = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    PSECURITY_DESCRIPTOR created = nullptr;
    const ULONG flags = SEF_AVOID_OWNER_CHECK | SEF_AVOID_PRIVILEGE_CHECK |
                        ((control & SE_DACL_AUTO_INHERITED) ? SEF_DACL_AUTO_INHERIT : 0);
    const BOOL ok = CreatePrivateObjectSecurityEx(parent.data(), nullptr, &created, nullptr, FALSE, flags, token, &mapping);
    const DWORD ce = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!ok) return ce;
    const BYTE* bytes = static_cast<const BYTE*>(created);
    sd->assign(bytes, bytes + GetSecurityDescriptorLength(created));
    DestroyPrivateObjectSecurity(&created);
    return ERROR_SUCCESS;
}

}  // namespace

DWORD write_file_atomically(const fs::path& path, const std::string& bytes, DWORD retry_ms, const fs::path& temp_dir) {
    std::error_code ec;
    const fs::path target = fs::absolute(path, ec);
    if (ec) return static_cast<DWORD>(ec.value());

    // Equalizer APO reloads on every name created in its config directory, so a
    // temporary file there costs a second reload. It is made outside, on the
    // same volume so the move is a rename, unless no such directory is at hand.
    fs::path dir = temp_dir;
    if (dir.empty()) {
        if (const DWORD e = default_temp_dir(&dir); e != ERROR_SUCCESS) return e;
    }
    const std::wstring volume = volume_guid_path(target.parent_path());
    const bool outside = !volume.empty() && _wcsicmp(volume.c_str(), volume_guid_path(dir).c_str()) == 0;

    // Moved in, the file keeps the DACL it was created with, so it is given the
    // one the file it replaces has, or a file created in the directory would get.
    std::vector<BYTE> sd;
    if (outside) {
        if (const DWORD e = dacl_for(target, &sd); e != ERROR_SUCCESS) return e;
        fs::create_directories(dir, ec);
        if (ec) return static_cast<DWORD>(ec.value());
        // Without the request bit, setting a DACL clears SE_DACL_AUTO_INHERITED.
        WORD control = 0;
        DWORD revision = 0;
        if (GetSecurityDescriptorControl(sd.data(), &control, &revision) && (control & SE_DACL_AUTO_INHERITED)) {
            SetSecurityDescriptorControl(sd.data(), SE_DACL_AUTO_INHERIT_REQ, SE_DACL_AUTO_INHERIT_REQ);
        }
    }

    // A name of its own for each process and thread, so writers running at the
    // same time never delete, open or rename each other's temporary file.
    fs::path tmp = (outside ? dir : target.parent_path()) / target.filename();
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";

    // CREATE_ALWAYS on an existing name writes through it, so a .tmp planted as
    // a hard link to another file would be overwritten. Remove the name, which
    // only unlinks it, and create a new file.
    DeleteFileW(tmp.c_str());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE | WRITE_DAC, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                     written == bytes.size());
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
    const bool flushed = ok && FlushFileBuffers(h);
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    const bool secured = !flushed || !outside || SetKernelObjectSecurity(h, DACL_SECURITY_INFORMATION, sd.data());
    const DWORD secure_error = secured ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!secured) {
        DeleteFileW(tmp.c_str());
        return secure_error;
    }
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
        const bool forgood = e == ERROR_ACCESS_DENIED && replace_denied_for_good(path);
        std::fprintf(stderr, "PROBE move failed e=%lu for_good=%d\n", e, static_cast<int>(forgood));
        const bool contended = e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ||
                               (e == ERROR_ACCESS_DENIED && !forgood);
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
    // If, EndIf and Stage lines included, and what is inside an If that is false.
    // Which devices match, and which conditions hold, only the device can say;
    // `Device: all` and no If reach every device.
    bool every_device = true;
    std::string device = "all";
    // The Ifs open under each Device pattern, as IfFilterFactory counts them on
    // a device the pattern matches: an If adds one, an EndIf takes one away if
    // any is open. Patterns compare in lower case, as matchDevice does, and are
    // otherwise taken as unrelated. An EndIf under a pattern with no If open
    // closes one only on the devices that also match another pattern, which is
    // known only under `Device: all` with Ifs open under one pattern; otherwise
    // the If stays counted, so attach writes an EndIf some devices log as one
    // without an If and ignore, rather than too few, which would hide the Include.
    auto& open_ifs = r.open_ifs_under;
    const auto find_pattern = [&](auto& list, const std::string& pattern) {
        return std::find_if(list.begin(), list.end(), [&](const auto& e) { return lower(e.first) == lower(pattern); });
    };
    const auto if_depth = [&] {
        unsigned n = 0;
        for (const auto& e : open_ifs) n += e.second;
        return n;
    };
    // Which Equalizer APO instances a line reaches after Stage lines, as the
    // values it can have: StageFilterFactory starts every configuration with
    // stageMatches = capture || !preMix || !postMixInstalled, and a Stage line
    // replaces that until the end of config.txt. Isotone.txt is for the post-mix
    // instance of a render device and for capture devices, and must not run a
    // second time in the pre-mix instance where post-mix is installed too. A
    // pre-mix instance with no post-mix installed matches by default, but no
    // Stage line says that without also matching pre-mix where post-mix is
    // installed, so that case is not modelled. Include is handled before Stage
    // (FilterEngine's factory order), but every line inside the included file
    // is skipped where the stage does not match.
    struct Reach {
        bool can_match, can_skip;
    };
    Reach post_mix{true, false}, capture{true, false}, pre_mix{false, true};
    // Whether every device's Stage lines leave it reaching what no Stage line
    // does: the last Stage line every device saw, then each Device pattern's
    // Stage lines after it. One inside an If, which some devices may skip, can
    // leave a pattern changed but cannot undo a change.
    bool stage_default_everywhere = true;
    std::vector<std::pair<std::string, bool>> stage_changed;   // pattern, left changed
    const auto stage_is_default = [&] {
        return stage_default_everywhere &&
               std::none_of(stage_changed.begin(), stage_changed.end(), [](const auto& e) { return e.second; });
    };
    for (const std::string& line : config_lines(bytes)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = line.substr(colon + 1);
        if (key == "Include") {
            r.includes.push_back(trim(value));
            if (names_isotone_file(value, config_dir)) {
                if (every_device && if_depth() == 0 && stage_is_default()) {
                    r.isotone_included = true;
                } else if (post_mix.can_match || capture.can_match || pre_mix.can_match) {
                    r.isotone_included_conditionally = true;
                }
            }
            r.peace_included |= names_peace_file(value);
        } else if (key == "Device") {
            device = trim(value);
            every_device = lower(device) == "all";
            if (every_device) device = "all";
        } else if (key == "Stage") {
            r.has_stage_lines = true;
            // StageFilterFactory: lower case, split on spaces, any part matching.
            bool post = false, cap = false, pre = false;
            std::istringstream parts(lower(trim(value)));
            for (std::string part; std::getline(parts, part, ' ');) {
                post |= part == "post-mix";
                cap |= part == "capture";
                pre |= part == "pre-mix";
            }
            // A Stage line some devices or conditions skip leaves either value.
            const bool everywhere = every_device && if_depth() == 0;
            for (auto [reach, matches] : {std::pair{&post_mix, post}, {&capture, cap}, {&pre_mix, pre}}) {
                *reach = everywhere ? Reach{matches, !matches}
                                    : Reach{reach->can_match || matches, reach->can_skip || !matches};
            }
            const bool changed = !(post && cap && !pre);
            if (everywhere) {
                stage_default_everywhere = !changed;
                stage_changed.clear();
            } else {
                auto it = find_pattern(stage_changed, device);
                if (it == stage_changed.end()) it = stage_changed.emplace(stage_changed.end(), device, false);
                it->second = if_depth() == 0 ? changed : it->second || changed;
            }
        } else if (key == "If" || key == "ElseIf" || key == "Else" || key == "EndIf") {
            r.has_conditionals = true;
            const auto it = find_pattern(open_ifs, device);
            if (key == "If") {
                if (it != open_ifs.end()) {
                    ++it->second;
                } else {
                    open_ifs.emplace_back(device, 1);
                }
            } else if (key == "EndIf") {
                // Under `Device: all` with Ifs open under one pattern only, it
                // closes one of those on every device that has one.
                auto closed = it;
                if (closed == open_ifs.end() && every_device && open_ifs.size() == 1) closed = open_ifs.begin();
                if (closed != open_ifs.end() && --closed->second == 0) open_ifs.erase(closed);
            }
        }
    }
    r.open_ifs = if_depth();
    r.stage_changed_at_end = !stage_is_default();
    // A change every device saw, or one under `Device: all`, is undone for all
    // devices; otherwise each pattern's is undone for its devices.
    const bool for_all = !stage_default_everywhere ||
                         std::any_of(stage_changed.begin(), stage_changed.end(),
                                     [](const auto& e) { return e.second && e.first == "all"; });
    for (const auto& [pattern, changed] : stage_changed) {
        if (changed && !for_all) r.stage_changed_under.push_back(pattern);
    }
    if (for_all) r.stage_changed_under.push_back("all");
    r.attached_by_isotone = attached_block_size(bytes) != 0;
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
    const std::string block =
        attach_block(newline_of(original), r.before, !original.empty() && original.back() != '\n');
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

    const size_t block = attached_block_size(bytes);
    if (block == 0) {
        CloseHandle(h);
        return inspect_config_text(bytes, config_dir).isotone_included ? ERROR_INVALID_DATA : ERROR_SUCCESS;
    }

    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(bytes.size() - block);
    const bool ok = SetFilePointerEx(h, end, nullptr, FILE_BEGIN) && SetEndOfFile(h) &&
                    FlushFileBuffers(h);
    const DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok) return e;
    *removed = true;
    return ERROR_SUCCESS;
}

}  // namespace isotone::compat
