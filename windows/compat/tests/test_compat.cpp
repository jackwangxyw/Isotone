// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The compatibility backend's file handling. Everything runs in a fresh
// directory under %TEMP%; nothing here knows where a real Equalizer APO lives.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "doctest.h"

#include <windows.h>

#include <aclapi.h>
#include <audioclient.h>
#include <objbase.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <atomic>
#include <chrono>
#include <optional>
#include <regex>
#include <sstream>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "compat_writer.h"
#include "config_files.h"
#include "eapo_install.h"
#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"
#include "isotone_file.h"
#include "loopback_capture.h"

namespace fs = std::filesystem;
using namespace isotone;
using namespace isotone::compat;

namespace {

struct Sandbox {
    Sandbox() {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        GUID g;
        CoCreateGuid(&g);
        wchar_t name[64];
        StringFromGUID2(g, name, 64);
        dir = fs::path(temp) / (std::wstring(L"isotone-compat-test-") + name);
        fs::create_directories(dir);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path dir;
};

void put(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string get(const fs::path& p) {
    std::string s;
    REQUIRE(read_file_bytes(p, &s) == ERROR_SUCCESS);
    return s;
}

// The owner's live config.txt, as snapshotted into sim/reference (20 bytes,
// CRLF). Written out literally so CI, which has no sim/, runs the same test.
constexpr char kOwnerConfig[] = "Include: peace.txt\r\n";

// The block attach appends, with `lines` between its comment and the Include,
// after a line break when the file's last line has none.
std::string attach_text(const char* nl, const std::vector<std::string>& lines, bool after_line_break = false) {
    static const char* const kWords[] = {"three", "four", "five", "six", "seven", "eight", "nine"};
    const size_t count = lines.size() + 2;
    std::string out = std::string(after_line_break ? nl : "") + "# Added by Isotone. Remove these " +
                      (count - 3 < 7 ? kWords[count - 3] : std::to_string(count)) + " lines" +
                      (after_line_break ? " and the line break before them" : "") + " to detach it." + nl;
    for (const std::string& line : lines) out += line + nl;
    return out + "Include: Isotone.txt" + nl;
}

// The same, for lines all under `Device: all`.
std::string block(const char* nl, const std::vector<std::string>& before_include = {}, bool after_line_break = false) {
    std::vector<std::string> lines{"Device: all"};
    lines.insert(lines.end(), before_include.begin(), before_include.end());
    return attach_text(nl, lines, after_line_break);
}

// Where write_file_atomically makes its temporary files by default.
fs::path default_temp_dir() {
    PWSTR local = nullptr;
    REQUIRE(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)));
    const fs::path dir = fs::path(local) / L"Isotone" / L"compat-tmp";
    CoTaskMemFree(local);
    return dir;
}

// Temporary files write_file_atomically may have left in `dir`, or left in the
// default temporary directory by this process (other processes share that one).
bool no_temporary_files(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".tmp") return false;
    }
    std::error_code ec;
    const std::wstring mine = L"." + std::to_wstring(GetCurrentProcessId()) + L".";
    for (const auto& entry : fs::directory_iterator(default_temp_dir(), ec)) {
        if (entry.path().extension() == ".tmp" && entry.path().filename().native().find(mine) != std::wstring::npos) {
            return false;
        }
    }
    return true;
}

// Records the changes made in a directory and below from construction on, with
// every filter ReadDirectoryChangesW has but security.
class DirectoryWatch {
public:
    struct Event {
        DWORD action;
        std::wstring name;   // relative to the directory; empty for a lost-events overflow
        bool operator==(const Event& o) const { return action == o.action && name == o.name; }
    };

    explicit DirectoryWatch(const fs::path& dir)
        : h_(CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr)) {
        REQUIRE(h_ != INVALID_HANDLE_VALUE);
        ov_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        arm();
    }
    ~DirectoryWatch() {
        CancelIoEx(h_, &ov_);
        DWORD n = 0;
        GetOverlappedResult(h_, &ov_, &n, TRUE);
        CloseHandle(ov_.hEvent);
        CloseHandle(h_);
    }

    // What arrived so far, once nothing more has arrived for `quiet_ms`.
    std::vector<Event> events(DWORD quiet_ms = 300) {
        while (WaitForSingleObject(ov_.hEvent, quiet_ms) == WAIT_OBJECT_0) {
            DWORD n = 0;
            REQUIRE(GetOverlappedResult(h_, &ov_, &n, FALSE));
            if (n == 0) events_.push_back({0, L""});
            for (size_t at = 0; n != 0;) {
                const auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer_ + at);
                events_.push_back({info->Action, std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t))});
                if (info->NextEntryOffset == 0) break;
                at += info->NextEntryOffset;
            }
            arm();
        }
        return events_;
    }

private:
    void arm() {
        ResetEvent(ov_.hEvent);
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                             FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        REQUIRE((ReadDirectoryChangesW(h_, buffer_, sizeof(buffer_), TRUE, filter, nullptr, &ov_, nullptr) ||
                 GetLastError() == ERROR_IO_PENDING));
    }

    HANDLE h_;
    OVERLAPPED ov_{};
    alignas(DWORD) BYTE buffer_[64 * 1024];
    std::vector<Event> events_;
};

std::string event_text(const std::vector<DirectoryWatch::Event>& events) {
    std::string out;
    for (const auto& e : events) {
        out += std::to_string(e.action) + ":";
        for (wchar_t c : e.name) out += static_cast<char>(c < 128 ? c : '?');
        out += " ";
    }
    return out;
}

// Owner, group and DACL.
std::string sddl_of(const fs::path& p) {
    const SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD need = 0;
    GetFileSecurityW(p.c_str(), si, nullptr, 0, &need);
    std::vector<BYTE> sd(need);
    if (need == 0 || !GetFileSecurityW(p.c_str(), si, sd.data(), need, &need)) return "error " + std::to_string(GetLastError());
    LPWSTR s = nullptr;
    REQUIRE(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.data(), SDDL_REVISION_1, si, &s, nullptr));
    std::string out;
    for (const wchar_t* c = s; *c; ++c) out += static_cast<char>(*c);
    LocalFree(s);
    return out;
}

// Sets a DACL the way Explorer and icacls do, so ACEs are inherited below it and
// marked as such: SE_DACL_AUTO_INHERITED, protected (`P`) or not.
void set_inheriting_dacl(const fs::path& p, const wchar_t* sddl, bool protect) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr));
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    std::wstring name = p.native();
    const DWORD e = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION
                                                                               : UNPROTECTED_DACL_SECURITY_INFORMATION),
                                          nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    REQUIRE(e == ERROR_SUCCESS);
}

// A directory on a drive letter nothing is mounted on.
fs::path unmounted_drive_dir() {
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter > L'D'; --letter) {
        if (!(drives & (1u << (letter - L'A')))) return std::wstring(1, letter) + L":\\isotone-compat-tmp";
    }
    FAIL("every drive letter is in use");
    return {};
}

// Runs isotone-compat with `args` and returns its exit code and output.
struct CliResult {
    DWORD exit_code = ~DWORD{0};
    std::string out;
};
CliResult run_cli(const std::vector<std::wstring>& args) {
    std::wstring cmd = L"\"" + fs::path(ISOTONE_COMPAT_EXE).wstring() + L"\"";
    for (const std::wstring& a : args) cmd += L" \"" + a + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    CliResult r;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return r;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(write_end);
    if (started) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &got, nullptr) && got > 0) r.out.append(buf, got);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &r.exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(read_end);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// The guard that keeps tools off a live install

TEST_CASE("a protected root that exists but cannot be identified protects everything") {
    // The guard compares file IDs; a root on a file system that reports none
    // must refuse, not wave every path through. The named-pipe root exists and
    // has no file ID.
    Sandbox s;
    const fs::path pipes = L"\\\\.\\pipe\\";
    REQUIRE(GetFileAttributesW(pipes.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(path_is_inside(s.dir, pipes));
    CHECK_FALSE(path_is_inside(s.dir, s.dir / "no-such-root"));
}

TEST_CASE("a protected directory is recognised however its path is spelled") {
    // The first version compared path text, and the short name
    // C:\PROGRA~1\EqualizerAPO\config walked straight past it onto the owner's
    // live install. Every spelling below must be caught.
    Sandbox s;
    const fs::path prot = s.dir / "Program Files" / "EqualizerAPO" / "config";
    fs::create_directories(prot / "sub");
    fs::create_directories(s.dir / "Program Files" / "EqualizerAPO" / "configX");
    fs::create_directories(s.dir / "other");

    CHECK(path_is_inside(prot, prot));
    CHECK(path_is_inside(prot.wstring() + L"\\", prot));
    CHECK(path_is_inside(prot.wstring() + L"\\\\", prot));
    std::wstring upper = prot.wstring();
    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
    CHECK(path_is_inside(upper, prot));
    CHECK(path_is_inside(prot / "sub" / "..", prot));
    CHECK(path_is_inside(prot / "sub", prot));
    CHECK(path_is_inside(prot / "not-created-yet" / "deeper", prot));
    std::wstring forward = prot.wstring();
    for (wchar_t& c : forward) if (c == L'\\') c = L'/';
    CHECK(path_is_inside(forward, prot));

    wchar_t short_name[MAX_PATH] = {};
    const DWORD n = GetShortPathNameW(prot.c_str(), short_name, MAX_PATH);
    if (n > 0 && n < MAX_PATH && _wcsicmp(short_name, prot.c_str()) != 0) {
        std::string shown;
        for (DWORD i = 0; i < n; ++i) shown += static_cast<char>(short_name[i] < 128 ? short_name[i] : '?');
        CAPTURE(shown);
        CHECK(path_is_inside(short_name, prot));
    } else {
        MESSAGE("8.3 names are disabled on this volume; short-name spelling not exercised");
    }

    const fs::path junction = s.dir / "link";
    const std::wstring cmd = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + prot.wstring() + L"\" >nul";
    if (_wsystem(cmd.c_str()) == 0 && fs::exists(junction)) {
        CHECK(path_is_inside(junction, prot));
        CHECK(path_is_inside(junction / "sub", prot));
        RemoveDirectoryW(junction.c_str());   // the junction only, not its target
    } else {
        MESSAGE("could not create a junction; junction spelling not exercised");
    }

    CHECK_FALSE(path_is_inside(s.dir / "other", prot));
    CHECK_FALSE(path_is_inside(s.dir / "Program Files" / "EqualizerAPO" / "configX", prot));
    CHECK_FALSE(path_is_inside(s.dir / "Program Files" / "EqualizerAPO", prot));
    CHECK_FALSE(path_is_inside(prot, s.dir / "does-not-exist"));
}

// ---------------------------------------------------------------------------
// Atomic writes

TEST_CASE("an atomic write creates, replaces, and leaves no temporary file") {
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "first\n") == ERROR_SUCCESS);
    CHECK(get(f) == "first\n");
    REQUIRE(write_file_atomically(f, "second, and longer\n") == ERROR_SUCCESS);
    CHECK(get(f) == "second, and longer\n");
    REQUIRE(write_file_atomically(f, "") == ERROR_SUCCESS);
    CHECK(get(f).empty());
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a reader holding the file blocks a replace, and the retry waits it out") {
    // Equalizer APO opens config files GENERIC_READ with FILE_SHARE_READ only
    // (FilterEngine::loadConfigFile). Holding the file that way must really
    // block MoveFileExW, or the retry is dead code.
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "old") == ERROR_SUCCESS);

    HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const DWORD blocked = write_file_atomically(f, "new", 0);
    CAPTURE(blocked);
    CHECK(blocked != ERROR_SUCCESS);
    CHECK(no_temporary_files(s.dir));

    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CloseHandle(held);
    });
    const DWORD waited = write_file_atomically(f, "new", 1000);
    release.join();
    CHECK(waited == ERROR_SUCCESS);
    CHECK(get(f) == "new");
    CHECK(no_temporary_files(s.dir));

    // A reader that allows delete (Isotone's own reads, a scanner) blocks a
    // replace too, with ERROR_ACCESS_DENIED, and is waited out the same way.
    held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(write_file_atomically(f, "blocked", 0) == ERROR_ACCESS_DENIED);
    std::thread release_again([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CloseHandle(held);
    });
    const DWORD waited_again = write_file_atomically(f, "newer", 1000);
    release_again.join();
    CHECK(waited_again == ERROR_SUCCESS);
    CHECK(get(f) == "newer");
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("atomic writes to one path from several threads at once each land whole") {
    // They used to share one temporary name: one writer's delete or create
    // failed while another held it (ERROR_FILE_EXISTS), or renamed it away.
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    constexpr int kRounds = 200;
    std::atomic<int> failures{0};
    std::atomic<DWORD> first_error{ERROR_SUCCESS};
    const auto writer = [&](char fill) {
        const std::string content(4096, fill);
        for (int i = 0; i < kRounds; ++i) {
            if (const DWORD e = write_file_atomically(f, content); e != ERROR_SUCCESS) {
                DWORD none = ERROR_SUCCESS;
                first_error.compare_exchange_strong(none, e);
                failures++;
            }
        }
    };
    std::thread a(writer, 'a'), b(writer, 'b'), c(writer, 'c');
    a.join();
    b.join();
    c.join();
    CAPTURE(first_error.load());
    CHECK(failures == 0);
    const std::string last = get(f);
    CHECK((last == std::string(4096, 'a') || last == std::string(4096, 'b') || last == std::string(4096, 'c')));
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a replace waits out a target another writer left pending deletion") {
    // MoveFileExW leaves the target pending deletion while the writer replacing
    // it holds its handle, and opening a file in that state answers
    // ERROR_ACCESS_DENIED, exactly as an ACL that denies delete does. Windows
    // gives no way to tell the two apart by error code, and treating it as
    // permanent made two writers racing on one path give up: one saw the
    // other's pending deletion and stopped (CI, 2026-09-20).
    //
    // Setting the delete disposition on an open handle marks the name pending
    // deletion straight away and keeps it there until the handle closes, which
    // is the state a concurrent MoveFileExW passes through.
    // FILE_FLAG_DELETE_ON_CLOSE does not: the file is only removed when the
    // last handle goes, and other opens succeed until then.
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "old") == ERROR_SUCCESS);

    HANDLE pending = CreateFileW(f.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(pending != INVALID_HANDLE_VALUE);
    FILE_DISPOSITION_INFO disposition{TRUE};
    REQUIRE(SetFileInformationByHandle(pending, FileDispositionInfo, &disposition, sizeof(disposition)));

    // The state really is the one being tested: an open is refused, and refused
    // with the code an ACL denying delete would give.
    HANDLE probe = CreateFileW(f.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD probe_error = GetLastError();
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    REQUIRE(probe == INVALID_HANDLE_VALUE);
    CHECK(probe_error == ERROR_ACCESS_DENIED);

    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CloseHandle(pending);
    });
    const DWORD e = write_file_atomically(f, "new", 2000);
    release.join();
    CHECK(e == ERROR_SUCCESS);
    CHECK(get(f) == "new");
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a replace that can never succeed fails at once instead of retrying") {
    // MoveFileExW answers ERROR_ACCESS_DENIED for a reader holding the file and
    // for these alike; only the reader goes away. Waiting out the retry each
    // time stalls every commit.
    Sandbox s;
    const auto timed = [](const fs::path& path) {
        const auto t0 = std::chrono::steady_clock::now();
        const DWORD e = write_file_atomically(path, "new", 2000);
        return std::make_pair(e, std::chrono::steady_clock::now() - t0);
    };
    const auto quick = std::chrono::milliseconds(500);

    const fs::path read_only = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(read_only, "old") == ERROR_SUCCESS);
    REQUIRE(SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_READONLY));
    auto [e1, t1] = timed(read_only);
    CHECK(e1 == ERROR_ACCESS_DENIED);
    CHECK(t1 < quick);
    CHECK(get(read_only) == "old");
    SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_NORMAL);

    const fs::path directory = s.dir / "Directory.txt";
    fs::create_directory(directory);
    auto [e2, t2] = timed(directory);
    CHECK(e2 == ERROR_ACCESS_DENIED);
    CHECK(t2 < quick);

    // An ACL that denies deleting the file, in a directory that denies deleting
    // its children.
    const fs::path locked_dir = s.dir / "acl";
    fs::create_directory(locked_dir);
    const fs::path locked = locked_dir / "Isotone.txt";
    REQUIRE(write_file_atomically(locked, "old") == ERROR_SUCCESS);
    const auto set_dacl = [](const fs::path& path, const wchar_t* sddl) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr));
        const BOOL ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd);
        LocalFree(sd);
        REQUIRE(ok);
    };
    set_dacl(locked, L"D:P(A;;0x1e01bf;;;WD)");       // everything but DELETE
    set_dacl(locked_dir, L"D:P(A;;0x1f01bf;;;WD)");   // everything but FILE_DELETE_CHILD
    auto [e3, t3] = timed(locked);
    set_dacl(locked_dir, L"D:(A;OICI;FA;;;WD)");
    set_dacl(locked, L"D:(A;;FA;;;WD)");
    CHECK(e3 == ERROR_ACCESS_DENIED);
    CHECK(t3 < quick);
    CHECK(get(locked) == "old");
    CHECK(no_temporary_files(locked_dir));
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("an atomic write that cannot land leaves the target as it was") {
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "keep me") == ERROR_SUCCESS);
    HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(write_file_atomically(f, "lost", 50) != ERROR_SUCCESS);
    CloseHandle(held);
    CHECK(get(f) == "keep me");
    CHECK(no_temporary_files(s.dir));

    CHECK(write_file_atomically(s.dir / "no-such-dir" / "Isotone.txt", "x") == ERROR_PATH_NOT_FOUND);
    CHECK_FALSE(fs::exists(s.dir / "no-such-dir"));
}

TEST_CASE("an atomic write changes no name in the target's directory but the target's") {
    // Equalizer APO reloads for every name that changes in its config directory:
    // a temporary file created there and renamed over Isotone.txt was two
    // reloads per write, measured.
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    for (const std::string when : {"created", "replaced"}) {
        CAPTURE(when);
        DirectoryWatch watch(s.dir);
        REQUIRE(write_file_atomically(f, when) == ERROR_SUCCESS);   // the default temporary directory
        const auto events = watch.events();
        CAPTURE(event_text(events));
        CHECK_FALSE(events.empty());
        for (const auto& e : events) CHECK(e.name == L"Isotone.txt");
        CHECK(get(f) == when);
    }
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a replaced file keeps its DACL, auto-inherited flag and all") {
    Sandbox s, temp;
    set_inheriting_dacl(s.dir, L"D:(A;OICI;FA;;;BU)(A;OICI;FR;;;LS)(A;OICIIO;GA;;;CO)(A;;FA;;;SY)", true);
    const fs::path f = s.dir / "Isotone.txt";
    put(f, "old");
    // One explicit ACE on top of the inherited ones.
    set_inheriting_dacl(f, L"D:(A;;FR;;;WD)", false);
    const std::string before = sddl_of(f);
    CAPTURE(before);
    REQUIRE(before.find("D:AI(A;;FR;;;WD)") != std::string::npos);
    REQUIRE(sddl_of(temp.dir) != sddl_of(s.dir));

    REQUIRE(write_file_atomically(f, "new", 200, temp.dir) == ERROR_SUCCESS);
    CHECK(get(f) == "new");
    CHECK(sddl_of(f) == before);
    CHECK(no_temporary_files(temp.dir));

    // A protected DACL stays protected.
    set_inheriting_dacl(f, L"D:(A;;FA;;;BU)(A;;FR;;;WD)", true);
    const std::string protected_before = sddl_of(f);
    REQUIRE(protected_before.find("D:PAI(") != std::string::npos);
    REQUIRE(write_file_atomically(f, "newer", 200, temp.dir) == ERROR_SUCCESS);
    CHECK(sddl_of(f) == protected_before);
}

TEST_CASE("a file an atomic write creates gets the DACL a file created in the directory gets") {
    // Two directories: one whose DACL is auto-inherited, as Explorer and
    // installers set them, and one set with SetFileSecurityW, whose children's
    // ACEs NTFS does not mark inherited.
    Sandbox s, temp;
    const wchar_t* const kDacl = L"D:(A;OICI;FA;;;BU)(A;OICI;FR;;;LS)(A;OICIIO;GA;;;CO)(A;;FA;;;SY)(A;CIIO;GA;;;NS)";
    const fs::path inheriting = s.dir / "inheriting";
    const fs::path plain = s.dir / "plain";
    fs::create_directory(inheriting);
    fs::create_directory(plain);
    set_inheriting_dacl(inheriting, kDacl, true);
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(kDacl, SDDL_REVISION_1, &sd, nullptr));
    const BOOL set = SetFileSecurityW(plain.c_str(), DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    REQUIRE(set);

    for (const fs::path& dir : {inheriting, plain}) {
        CAPTURE(sddl_of(dir));
        put(dir / "direct.txt", "x");
        const std::string direct = sddl_of(dir / "direct.txt");
        CAPTURE(direct);
        REQUIRE(write_file_atomically(dir / "Isotone.txt", "x", 200, temp.dir) == ERROR_SUCCESS);
        CHECK(sddl_of(dir / "Isotone.txt") == direct);
    }
    CHECK(sddl_of(inheriting / "Isotone.txt").find("D:AI(") != std::string::npos);
    CHECK(no_temporary_files(temp.dir));
}

TEST_CASE("a temporary directory on another volume falls back to a temporary file next to the target") {
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    // A drive letter nothing is mounted on has no volume to match.
    std::vector<fs::path> elsewhere = {unmounted_drive_dir()};
    // A junction on this volume to a directory on another one that does not
    // exist: the volume is real and different, and nothing can be created there.
    const DWORD drives = GetLogicalDrives();
    const fs::path junction = s.dir / "other-volume";
    const wchar_t own[4] = {s.dir.native()[0], L':', L'\\', 0};
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
        const wchar_t root[4] = {letter, L':', L'\\', 0};
        if (!(drives & (1u << (letter - L'A'))) || _wcsicmp(root, own) == 0 || GetDriveTypeW(root) != DRIVE_FIXED) continue;
        const std::wstring cmd = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + root +
                                 L"isotone-no-such-directory\" >nul";
        if (_wsystem(cmd.c_str()) == 0) elsewhere.push_back(junction / "compat-tmp");
        break;
    }
    if (elsewhere.size() < 2) MESSAGE("no other fixed volume; the junction case is not exercised");

    for (const fs::path& temp : elsewhere) {
        CAPTURE(temp.string());
        DirectoryWatch watch(s.dir);
        REQUIRE(write_file_atomically(f, temp.string(), 200, temp) == ERROR_SUCCESS);
        CHECK(get(f) == temp.string());
        const auto events = watch.events();
        CAPTURE(event_text(events));
        const std::wstring tmp_name = L"Isotone.txt." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                      std::to_wstring(GetCurrentThreadId()) + L".tmp";
        CHECK(std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.name == tmp_name; }));
        CHECK(no_temporary_files(s.dir));
    }
    RemoveDirectoryW(junction.c_str());   // the junction only
}

// ---------------------------------------------------------------------------
// The Include line

TEST_CASE("the literal matches the snapshot of the owner's config.txt") {
    const fs::path snapshot = ISOTONE_SIM_REFERENCE "/apo-live/config.txt";
    if (!fs::exists(snapshot)) {
        MESSAGE("no sim/reference in this checkout; literal not cross-checked");
        return;
    }
    CHECK(get(snapshot) == kOwnerConfig);
}

TEST_CASE("attaching to the owner's config appends once and detaches back to the same bytes") {
    Sandbox s;
    const fs::path config = s.dir / "config.txt";
    put(config, kOwnerConfig);

    const AttachResult first = attach_include(s.dir);
    REQUIRE(first.error == ERROR_SUCCESS);
    CHECK(first.appended);
    CHECK(first.before.peace_included);
    CHECK_FALSE(first.before.isotone_included);
    CHECK(get(config) == std::string(kOwnerConfig) + block("\r\n"));
    CHECK(get(s.dir / kConfigBackupName) == kOwnerConfig);

    const ConfigInspection after = inspect_config(s.dir);
    CHECK(after.isotone_included);
    CHECK(after.attached_by_isotone);

    const AttachResult second = attach_include(s.dir);
    REQUIRE(second.error == ERROR_SUCCESS);
    CHECK_FALSE(second.appended);
    CHECK(get(config) == std::string(kOwnerConfig) + block("\r\n"));
    CHECK(get(s.dir / kConfigBackupName) == kOwnerConfig);

    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(config) == kOwnerConfig);

    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK_FALSE(removed);
    CHECK(get(config) == kOwnerConfig);
}

TEST_CASE("every byte of a busy config survives, whatever its line endings and last line") {
    // Peace, Convolution, VST, a second include, a scoped Device and Channel, a
    // Stage line, a UTF-8 BOM, non-ASCII text, LF endings and no final newline.
    Sandbox s;
    const std::string original =
        "\xEF\xBB\xBF# Peace-managed configuration \xE2\x80\x94 do not edit\n"
        "Include: peace.txt\n"
        "Device: Speakers Realtek\n"
        "Channel: L\n"
        "Convolution: C:\\Impulses\\room.wav\n"
        "VSTPlugin: Library C:\\VST\\plugin.dll ChunkData \"AAAA==\"\n"
        "Stage: post-mix\n"
        "Include: C:\\Users\\someone\\extra.txt\n"
        "Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1";
    put(s.dir / "config.txt", original);

    const AttachResult r = attach_include(s.dir);
    REQUIRE(r.error == ERROR_SUCCESS);
    CHECK(r.appended);
    CHECK(r.before.peace_included);
    CHECK(r.before.has_stage_lines);
    CHECK_FALSE(r.before.has_conditionals);
    CHECK(r.before.includes.size() == 2);

    // `Stage: post-mix` leaves the end of the file out of reach of capture
    // devices its Device line matches, so the block undoes it for them before
    // the Include.
    const std::string after = get(s.dir / "config.txt");
    const std::string appended =
        attach_text("\n", {"Device: Speakers Realtek", "Stage: post-mix capture", "Device: all"}, true);
    REQUIRE(after.size() == original.size() + appended.size());
    CHECK(after.compare(0, original.size(), original) == 0);
    CHECK(after.substr(original.size()) == appended);

    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(s.dir / "config.txt") == original);
}

TEST_CASE("an Include is recognised the way upstream reads it") {
    Sandbox s;
    // Upstream compares the command case-sensitively, so this line is not an
    // include to Equalizer APO and must not stop Isotone attaching.
    put(s.dir / "config.txt", "include: Isotone.txt\r\n");
    CHECK_FALSE(inspect_config(s.dir).isotone_included);
    CHECK(attach_include(s.dir).appended);

    // A hand-written relative include of our file is ours already.
    put(s.dir / "config.txt", "Preamp: -2 dB\r\n  Include:   .\\ISOTONE.TXT  \r\n");
    const AttachResult r = attach_include(s.dir);
    REQUIRE(r.error == ERROR_SUCCESS);
    CHECK_FALSE(r.appended);
    CHECK(get(s.dir / "config.txt") == "Preamp: -2 dB\r\n  Include:   .\\ISOTONE.TXT  \r\n");

    // But detach will not touch an include it did not write.
    bool removed = true;
    CHECK(detach_include(s.dir, &removed) == ERROR_INVALID_DATA);
    CHECK_FALSE(removed);

    put(s.dir / "config.txt", "If: 1 == 1\r\nEndIf:\r\n");
    CHECK(inspect_config(s.dir).has_conditionals);

    // An include that only some devices reach is not attached for all of them,
    // and attaching again would include the file twice for the ones it does.
    for (const char* scoped : {"Device: Speakers\r\nInclude: Isotone.txt\r\n",
                               "If: sampleRate == 44100\r\nInclude: Isotone.txt\r\nEndIf:\r\n"}) {
        CAPTURE(scoped);
        put(s.dir / "config.txt", scoped);
        const ConfigInspection i = inspect_config(s.dir);
        CHECK_FALSE(i.isotone_included);
        CHECK(i.isotone_included_conditionally);
        const AttachResult refused = attach_include(s.dir);
        CHECK(refused.error != ERROR_SUCCESS);
        CHECK_FALSE(refused.appended);
        CHECK(get(s.dir / "config.txt") == scoped);
    }
    put(s.dir / "config.txt", "Device: Speakers\r\nPreamp: -3 dB\r\nDevice: all\r\nInclude: Isotone.txt\r\n");
    CHECK(inspect_config(s.dir).isotone_included);
    CHECK_FALSE(inspect_config(s.dir).isotone_included_conditionally);
}

TEST_CASE("detach removes the block only while it is still the file's tail") {
    Sandbox s;
    put(s.dir / "config.txt", kOwnerConfig);
    REQUIRE(attach_include(s.dir).appended);

    // An edit above the block leaves it detachable, and the edit stays.
    const std::string edited = std::string("# my note\r\n") + kOwnerConfig;
    put(s.dir / "config.txt", edited + block("\r\n"));
    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(s.dir / "config.txt") == edited);

    // A line added after the block means the user changed it; hands off.
    REQUIRE(attach_include(s.dir).appended);
    const std::string after_block = get(s.dir / "config.txt") + "Preamp: -1 dB\r\n";
    put(s.dir / "config.txt", after_block);
    CHECK(detach_include(s.dir, &removed) == ERROR_INVALID_DATA);
    CHECK_FALSE(removed);
    CHECK(get(s.dir / "config.txt") == after_block);
}

TEST_CASE("an empty config.txt gets CRLF and nothing else") {
    Sandbox s;
    put(s.dir / "config.txt", "");
    REQUIRE(attach_include(s.dir).appended);
    CHECK(get(s.dir / "config.txt") == block("\r\n"));
}

TEST_CASE("missing or malformed config directories fail without creating anything") {
    Sandbox s;
    const fs::path missing = s.dir / "nope";
    CHECK(inspect_config(missing).error == ERROR_PATH_NOT_FOUND);
    CHECK(attach_include(missing).error == ERROR_PATH_NOT_FOUND);
    bool removed = false;
    CHECK(detach_include(missing, &removed) == ERROR_PATH_NOT_FOUND);
    CHECK_FALSE(fs::exists(missing));

    // A directory with no config.txt: not an install; do not invent one.
    CHECK(attach_include(s.dir).error == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(fs::exists(s.dir / "config.txt"));
    CHECK_FALSE(fs::exists(s.dir / kConfigBackupName));

    fs::create_directory(s.dir / "config.txt");
    CHECK(attach_include(s.dir).error == ERROR_DIRECTORY);

    CompatWriter writer(missing);
    CHECK(writer.load() == ERROR_FILE_NOT_FOUND);   // the directory itself is what is missing
}

// ---------------------------------------------------------------------------
// Isotone.txt

namespace {

constexpr uint32_t kSpeaker714 = 0x2D63F;
constexpr char kStereo[] = "{798436D2-8C71-4834-9248-00CCBAACA00A}";
constexpr char kHeight[] = "{11111111-2222-3333-4444-555555555555}";

Band band(FilterType type, double fc, double gain, double width, WidthMode mode,
          ChannelMask channels = kAllChannels, bool corner = false) {
    Band b;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain;
    b.width = width;
    b.width_mode = mode;
    b.channels = channels;
    b.shelf_corner = corner;
    return b;
}

DeviceConfig stereo_device() {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {2, default_speaker_mask(2)};
    d.state.preamp_db = -6.1;
    d.state.bands.push_back(band(FilterType::LowShelf, 105, 6.4, 0.7, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::Peaking, 1000.5, -3.25, 1.42, WidthMode::Q, 1u << 1));
    d.state.bands.push_back(band(FilterType::HighShelf, 3000, 4, 0.9 * 12, WidthMode::SlopeDb));
    d.state.bands.push_back(band(FilterType::Peaking, 250, 2, 0.5, WidthMode::BandwidthOct));
    d.state.channel_gain_db[0] = -1.5;
    d.state.speakers.delay_ms[1] = 2.5;
    d.state.speakers.inverted = 1u << 0;
    d.state.speakers.swap_left_right = true;
    return d;
}

DeviceConfig height_device() {
    DeviceConfig d;
    d.endpoint_guid = kHeight;
    d.layout = {12, kSpeaker714};
    d.state.bands.push_back(band(FilterType::Peaking, 8000, -6, 2, WidthMode::Q, 1u << 10));
    d.state.bands.push_back(band(FilterType::LowShelf, 80, 3, 0.7, WidthMode::Q, 1u << 3, true));
    d.state.channel_gain_db[7] = 2;
    d.state.mute = true;
    return d;
}

ChannelLayout layout_for(const std::string& guid) {
    if (guid.find("11111111") != std::string::npos) return {12, kSpeaker714};
    return {2, default_speaker_mask(2)};
}

void check_same_state(const EqState& a, const EqState& b) {
    CHECK(a.bypass == b.bypass);
    CHECK(a.mute == b.mute);
    CHECK(format_speaker_setup(a.speakers) == format_speaker_setup(b.speakers));
    CHECK(a.preamp_db == b.preamp_db);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        CAPTURE(c);
        CHECK(a.channel_gain_db[c] == b.channel_gain_db[c]);
    }
    // format_apo_config writes bands grouped by channel mask, so they come back
    // in group order. A cascade of linear filters has the same response in any
    // order, so compare them as a set.
    REQUIRE(a.bands.size() == b.bands.size());
    const auto sorted = [](std::vector<Band> v) {
        std::sort(v.begin(), v.end(), [](const Band& x, const Band& y) {
            return std::tie(x.channels, x.fc, x.gain_db) < std::tie(y.channels, y.fc, y.gain_db);
        });
        return v;
    };
    const std::vector<Band> sa = sorted(a.bands), sb = sorted(b.bands);
    for (size_t i = 0; i < sa.size(); ++i) {
        CAPTURE(i);
        CHECK(sa[i].type == sb[i].type);
        CHECK(sa[i].fc == sb[i].fc);
        CHECK(sa[i].gain_db == sb[i].gain_db);
        CHECK(sa[i].width == sb[i].width);
        CHECK(sa[i].width_mode == sb[i].width_mode);
        CHECK(sa[i].channels == sb[i].channels);
        CHECK(sa[i].shelf_corner == sb[i].shelf_corner);
        CHECK(sa[i].enabled == sb[i].enabled);
    }
}

}  // namespace

TEST_CASE("Isotone.txt re-parses to the same state for every device") {
    const DeviceConfig a = stereo_device();
    const DeviceConfig b = height_device();
    const std::string text = update_isotone_file(update_isotone_file("", a), b);
    MESSAGE(text);

    const std::vector<ParsedDevice> parsed = parse_isotone_file(text, layout_for);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].endpoint_guid == kStereo);
    CHECK(parsed[1].endpoint_guid == kHeight);
    for (size_t i = 0; i < 2; ++i) {
        CAPTURE(i);
        CHECK(parsed[i].warnings.empty());
        CHECK(parsed[i].unsupported.empty());
    }
    check_same_state(parsed[0].state, a.state);
    check_same_state(parsed[1].state, b.state);

    SUBCASE("bypassed, the curve is kept, and upstream plays no preamp and no bands") {
        DeviceConfig off = a;
        off.state.bypass = true;
        const std::string bypassed = update_isotone_file(text, off);
        const std::vector<ParsedDevice> back = parse_isotone_file(bypassed, layout_for);
        REQUIRE(back.size() == 2);
        CHECK(back[0].warnings.empty());
        check_same_state(back[0].state, off.state);

        // Upstream's view: no band is a command, and the one Preamp left is the
        // trim under its Channel line.
        const std::string block_text = format_device_block(off);
        std::istringstream in(block_text);
        std::string line, previous;
        int filters = 0, preamps = 0;
        while (std::getline(in, line)) {
            if (line.rfind("Filter ", 0) == 0) ++filters;
            if (line.rfind("Preamp:", 0) == 0) {
                ++preamps;
                CHECK(previous == "Channel: L");
            }
            previous = line;
        }
        CHECK(filters == 0);
        CHECK(preamps == 1);
    }

    SUBCASE("a bypassed block written before bypass left the speaker setup on still reads") {
        const std::string old =
            "Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n# Isotone: bypass\n"
            "# Preamp: -3 dB\n# Filter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n";
        const std::vector<ParsedDevice> back = parse_isotone_file(old, layout_for);
        REQUIRE(back.size() == 1);
        CHECK(back[0].warnings.empty());
        CHECK(back[0].state.bypass);
        CHECK(back[0].state.preamp_db == -3.0);
        CHECK(back[0].state.bands.size() == 1);
    }
}

TEST_CASE("the file speaks only upstream's commands") {
    const std::string text =
        update_isotone_file(update_isotone_file("", stereo_device()), height_device());
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::string key = line.substr(0, line.find(':'));
        CAPTURE(line);
        CHECK((key == "Device" || key == "Channel" || key == "Preamp" || key == "Copy" ||
               key == "Delay" || key == "Filter" || key.rfind("Filter ", 0) == 0 || key == "If" || key == "EndIf"));
    }
    CHECK(text.find("Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n") !=
          std::string::npos);
}

namespace {

// Upstream Equalizer APO's own code for the parts of a line the model has to
// understand, ported from its source at the pinned commit
// (windows/devicetool/upstream/VENDORED.md) and using nothing from core/, so a
// mistake shared by Isotone's parser, filter design and channel naming shows
// up as a difference from the processor instead of passing on both sides.
namespace upstream {

// helpers/ChannelHelper.cpp: channelPosToNameMap, getDefaultChannelMask,
// getChannelNames.
int default_channel_mask(int channelCount) {
    switch (channelCount) {
        case 1: return 0x4;     // KSAUDIO_SPEAKER_MONO
        case 2: return 0x3;     // KSAUDIO_SPEAKER_STEREO
        case 4: return 0x33;    // KSAUDIO_SPEAKER_QUAD
        case 6: return 0x60F;   // KSAUDIO_SPEAKER_5POINT1_SURROUND
        case 8: return 0x63F;   // KSAUDIO_SPEAKER_7POINT1_SURROUND
        default: return 0;
    }
}

std::vector<std::string> channel_names(int channelCount, int channelMask) {
    static const std::map<int, std::string> kPosToName = {
        {0x1, "L"}, {0x2, "R"}, {0x4, "C"}, {0x8, "LFE"}, {0x10, "RL"}, {0x20, "RR"}, {0x100, "RC"},
        {0x200, "SL"}, {0x400, "SR"}};
    std::vector<std::string> channelNames;
    int c = 1;
    for (int i = 0; i < 31; i++) {
        const int channelPos = 1 << i;
        if (channelMask & channelPos) {
            const auto it = kPosToName.find(channelPos);
            channelNames.push_back(it != kPosToName.end() ? it->second : std::to_string(c));
            c++;
        }
    }
    for (; c <= channelCount; c++) channelNames.push_back(std::to_string(c));
    return channelNames;
}

// ChannelHelper::getChannelIndex; -1 for a word that names no channel.
long channel_index(const std::string& word, const std::vector<std::string>& channelNames) {
    if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0]))) {
        const long channelIndex = std::strtol(word.c_str(), nullptr, 10) - 1;
        return channelIndex < 0 || channelIndex >= static_cast<long>(channelNames.size()) ? -1 : channelIndex;
    }
    auto pos = std::find(channelNames.begin(), channelNames.end(), word);
    if (pos == channelNames.end()) {
        if (word == "SL") pos = std::find(channelNames.begin(), channelNames.end(), "RL");
        else if (word == "SR") pos = std::find(channelNames.begin(), channelNames.end(), "RR");
        else if (word == "RL") pos = std::find(channelNames.begin(), channelNames.end(), "SL");
        else if (word == "RR") pos = std::find(channelNames.begin(), channelNames.end(), "SR");
        else if (word == "SUB") pos = std::find(channelNames.begin(), channelNames.end(), "LFE");
    }
    return pos != channelNames.end() ? static_cast<long>(pos - channelNames.begin()) : -1;
}

// filters/BiQuad.h and BiQuad.cpp.
class BiQuad {
public:
    enum Type { LOW_PASS, HIGH_PASS, BAND_PASS, NOTCH, ALL_PASS, PEAKING, LOW_SHELF, HIGH_SHELF };

    BiQuad(Type type, double dbGain, double freq, double srate, double bandwidthOrQOrS, bool isBandwidthOrS) {
        constexpr double M_PI_ = 3.14159265358979323846;
        constexpr double M_LN2_ = 0.693147180559945309417;
        double A;
        if (type == PEAKING || type == LOW_SHELF || type == HIGH_SHELF)
            A = pow(10, dbGain / 40);
        else
            A = pow(10, dbGain / 20);
        double omega = 2 * M_PI_ * freq / srate;
        double sn = sin(omega);
        double cs = cos(omega);
        double alpha;

        if (!isBandwidthOrS)   // Q
            alpha = sn / (2 * bandwidthOrQOrS);
        else if (type == LOW_SHELF || type == HIGH_SHELF)   // S
            alpha = sn / 2 * sqrt((A + 1 / A) * (1 / bandwidthOrQOrS - 1) + 2);
        else   // BW
            alpha = sn * sinh(M_LN2_ / 2 * bandwidthOrQOrS * omega / sn);

        double beta = 2 * sqrt(A) * alpha;

        double b0 = 0, b1 = 0, b2 = 0, a0_ = 1, a1 = 0, a2 = 0;
        switch (type) {
            case LOW_PASS:
                b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
                a0_ = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
                break;
            case HIGH_PASS:
                b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
                a0_ = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
                break;
            case BAND_PASS:
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0_ = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
                break;
            case NOTCH:
                b0 = 1; b1 = -2 * cs; b2 = 1;
                a0_ = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
                break;
            case ALL_PASS:
                b0 = 1 - alpha; b1 = -2 * cs; b2 = 1 + alpha;
                a0_ = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
                break;
            case PEAKING:
                b0 = 1 + (alpha * A); b1 = -2 * cs; b2 = 1 - (alpha * A);
                a0_ = 1 + (alpha / A); a1 = -2 * cs; a2 = 1 - (alpha / A);
                break;
            case LOW_SHELF:
                b0 = A * ((A + 1) - (A - 1) * cs + beta);
                b1 = 2 * A * ((A - 1) - (A + 1) * cs);
                b2 = A * ((A + 1) - (A - 1) * cs - beta);
                a0_ = (A + 1) + (A - 1) * cs + beta;
                a1 = -2 * ((A - 1) + (A + 1) * cs);
                a2 = (A + 1) + (A - 1) * cs - beta;
                break;
            case HIGH_SHELF:
                b0 = A * ((A + 1) + (A - 1) * cs + beta);
                b1 = -2 * A * ((A - 1) + (A + 1) * cs);
                b2 = A * ((A + 1) + (A - 1) * cs - beta);
                a0_ = (A + 1) - (A - 1) * cs + beta;
                a1 = 2 * ((A - 1) - (A + 1) * cs);
                a2 = (A + 1) - (A - 1) * cs - beta;
                break;
        }

        a0 = b0 / a0_;
        a[0] = b1 / a0_;
        a[1] = b2 / a0_;
        a[2] = a1 / a0_;
        a[3] = a2 / a0_;
    }

    double process(double sample) {
        double result = a0 * sample + a[1] * x2 + a[0] * x1 - a[3] * y2 - a[2] * y1;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = result;
        return result;
    }

private:
    double a[4];
    double a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

// filters/BiQuadFilterFactory.cpp, getFreq.
double get_freq(const std::string& freqString) {
    double result;
    // (U+00A0 removal omitted: Isotone never writes it.)
    const std::string& s = freqString;
    if (sscanf_s(s.c_str(), "%lf", &result) == 1) {
        if (s.length() >= 5 && s.find_first_of("eE") == std::string::npos) {
            if (s[s.length() - 4] == '.') {
                // Interpret as thousands separator because of Room EQ Wizard
                result *= 1000.0;
            }
        }
        return result;
    }
    return -1.0;
}

// What BiQuadFilterFactory::createFilter passes to the BiQuadFilter it creates.
struct BiQuadFilterSpec {
    BiQuad::Type type;
    double dbGain, freq, bandwidthOrQOrS;
    bool isBandwidthOrS, isCornerFreq;
};

// BiQuadFilterFactory::createFilter; nothing when upstream creates no filter.
// The regexes are upstream's, in narrow form, without U+00A0 in regexFreq.
std::optional<BiQuadFilterSpec> create_filter(const std::string& command, std::string parameters) {
    static const std::regex regexType(R"(^\s*ON\s+([A-Za-z]+))");
    static const std::regex regexFreq(R"(\s+Fc\s*([-+0-9.eE]+)\s*H\s*z)");
    static const std::regex regexGain(R"(\s+Gain\s*([-+0-9.eE]+)\s*dB)");
    static const std::regex regexQ(R"(\s+Q\s*([-+0-9.eE]+))");
    static const std::regex regexBW(R"(\s+BW\s+Oct\s*([-+0-9.eE]+))");
    static const std::regex regexSlope(R"(^\s*([-+0-9.eE]+)\s*dB)");
    static const std::map<std::string, BiQuad::Type> filterNameToTypeMap = {
        {"PK", BiQuad::PEAKING},    {"PEQ", BiQuad::PEAKING},    {"Modal", BiQuad::PEAKING},
        {"LP", BiQuad::LOW_PASS},   {"HP", BiQuad::HIGH_PASS},   {"LPQ", BiQuad::LOW_PASS},
        {"HPQ", BiQuad::HIGH_PASS}, {"BP", BiQuad::BAND_PASS},   {"LS", BiQuad::LOW_SHELF},
        {"HS", BiQuad::HIGH_SHELF}, {"LSC", BiQuad::LOW_SHELF},  {"HSC", BiQuad::HIGH_SHELF},
        {"NO", BiQuad::NOTCH},      {"AP", BiQuad::ALL_PASS}};

    if (command.find("Filter") != 0) return std::nullopt;
    // Conversion to period as decimal mark, if needed
    std::replace(parameters.begin(), parameters.end(), ',', '.');

    std::smatch match;
    if (!std::regex_search(parameters, match, regexType)) return std::nullopt;
    const std::string typeString = match.str(1);
    const auto found_type = filterNameToTypeMap.find(typeString);
    if (found_type == filterNameToTypeMap.end()) return std::nullopt;
    const BiQuad::Type type = found_type->second;
    parameters = match.suffix().str();

    double freq = 0;
    double gain = 0;
    double bandwidthOrQOrS = 0;
    bool isBandwidthOrS = false;
    bool isCornerFreq = false;
    bool error = false;

    if (std::regex_search(parameters, match, regexFreq)) {
        freq = get_freq(match.str(1));
    } else {
        error = true;
    }

    if (std::regex_search(parameters, match, regexGain)) {
        if (!(type == BiQuad::LOW_PASS || type == BiQuad::HIGH_PASS || type == BiQuad::NOTCH || type == BiQuad::ALL_PASS)) {
            gain = std::strtod(match.str(1).c_str(), nullptr);
        }
    } else if (type == BiQuad::PEAKING || type == BiQuad::LOW_SHELF || type == BiQuad::HIGH_SHELF) {
        error = true;
    }

    if (std::regex_search(parameters, match, regexQ)) {
        bandwidthOrQOrS = std::strtod(match.str(1).c_str(), nullptr);
    }

    if (std::regex_search(parameters, match, regexBW)) {
        if (!(type == BiQuad::LOW_SHELF || type == BiQuad::HIGH_SHELF)) {
            bandwidthOrQOrS = std::strtod(match.str(1).c_str(), nullptr);
            isBandwidthOrS = true;
        }
    }

    if (std::regex_search(parameters, match, regexSlope)) {
        if (type == BiQuad::LOW_SHELF || type == BiQuad::HIGH_SHELF) {
            bandwidthOrQOrS = std::strtod(match.str(1).c_str(), nullptr);
            isBandwidthOrS = true;
        }
    }

    if (bandwidthOrQOrS == 0) {
        if (type == BiQuad::PEAKING || type == BiQuad::ALL_PASS) {
            error = true;
        } else if (type == BiQuad::LOW_PASS || type == BiQuad::HIGH_PASS || type == BiQuad::BAND_PASS) {
            bandwidthOrQOrS = std::sqrt(0.5);   // M_SQRT1_2
        } else if (type == BiQuad::LOW_SHELF || type == BiQuad::HIGH_SHELF) {
            bandwidthOrQOrS = 0.9;   // found out by experimentation with RoomEQWizard
            isBandwidthOrS = true;
        } else if (type == BiQuad::NOTCH) {
            bandwidthOrQOrS = 30.0;   // found out by experimentation with RoomEQWizard
        }
    } else if (type == BiQuad::LOW_SHELF || type == BiQuad::HIGH_SHELF) {
        if (isBandwidthOrS)
            // Maximum S is 1 for 12 dB
            bandwidthOrQOrS /= 12.0;
        if (typeString[typeString.length() - 1] != 'C') isCornerFreq = true;
    }

    if (error) return std::nullopt;
    return BiQuadFilterSpec{type, gain, freq, bandwidthOrQOrS, isBandwidthOrS, isCornerFreq};
}

// filters/BiQuadFilter.cpp, initialize: the corner frequency shift, then one
// BiQuad per channel.
BiQuad make_biquad(const BiQuadFilterSpec& f, double sampleRate) {
    double biquadFreq = f.freq;
    if (f.isCornerFreq && (f.type == BiQuad::LOW_SHELF || f.type == BiQuad::HIGH_SHELF)) {
        double s = f.bandwidthOrQOrS;
        if (!f.isBandwidthOrS) {   // Q
            double q = f.bandwidthOrQOrS;
            double a = pow(10, f.dbGain / 40);
            s = 1.0 / ((1.0 / (q * q) - 2.0) / (a + 1.0 / a) + 1.0);
        }
        // frequency adjustment for DCX2496
        double centerFreqFactor = pow(10.0, std::abs(f.dbGain) / 80.0 / s);
        if (f.type == BiQuad::LOW_SHELF)
            biquadFreq *= centerFreqFactor;
        else
            biquadFreq /= centerFreqFactor;
    }
    return BiQuad(f.type, f.dbGain, biquadFreq, sampleRate, f.bandwidthOrQOrS, f.isBandwidthOrS);
}

// filters/DeviceFilterFactory.cpp, matchDevice.
bool match_device(const std::string& deviceString, const std::string& pattern) {
    const auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::string value = pattern;
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);
    value += ";";
    std::vector<std::vector<std::string>> fullList;
    std::vector<std::string> currentList;
    std::string currentWord;
    for (char c : value) {
        if (c == ' ' || c == ';') {
            if (!currentWord.empty()) {
                currentList.push_back(currentWord);
                currentWord.clear();
            }
            if (c == ';' && !currentList.empty()) {
                fullList.push_back(currentList);
                currentList.clear();
            }
        } else {
            currentWord += c;
        }
    }
    const std::string deviceStringNoGuid = std::regex_replace(
        deviceString, std::regex(R"(\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\})"), "");
    bool matches = false;
    for (const auto& list : fullList) {
        matches = true;
        if (list.size() == 1 && lower(list[0]) == "all") break;
        for (const std::string& w : list) {
            const std::string word = lower(w);
            const std::string& matchString = word.find('{') == std::string::npos ? deviceStringNoGuid : deviceString;
            if (lower(matchString).find(word) == std::string::npos) {
                matches = false;
                break;
            }
        }
        if (matches) break;
    }
    return matches;
}

}  // namespace upstream

// Runs a configuration the way upstream's FilterEngine does, for the commands
// Isotone writes and the lines that decide what applies, so text can be
// checked against the processor without an Equalizer APO install. Per line,
// the factories in FilterEngine's order: Device, If, Include, Stage, then the
// command. Semantics as read in upstream at the pinned commit:
//   Device   matches by DeviceFilterFactory::matchDevice; skips what follows
//            until the next Device line, and is reset at the end of a file.
//   If       IfFilterFactory's true and false counts, per file; an If left open
//            is closed at the end of its file, and an EndIf with none open is
//            counted. Only the forms of expression Isotone and these tests
//            write are evaluated.
//   Include  loads the named file (from `files`), restoring the channel
//            selection afterwards. It comes before Stage, so a file included
//            under a Stage that does not match is loaded but none of its
//            commands apply.
//   Stage    StageFilterFactory: starts as capture || !preMix ||
//            !postMixInstalled; a Stage line matches if any part names this
//            instance; kept per file.
//   Channel  selects channels by name or number, or all of them, virtual
//            channels included (ChannelFilter, ChannelHelper).
//   Preamp   float gain on the selection (PreampFilter).
//   Filter   one upstream BiQuad per selected channel (BiQuadFilterFactory).
//   Copy     every target computed from the inputs before any is written; a
//            target that is not a channel becomes a virtual channel starting at
//            zero; channels that are not targets keep their samples; a source
//            naming no channel is added as its factor, a constant (CopyFilter,
//            FilterConfiguration::process).
//   Delay    whole samples, rate * ms / 1000 + 0.5, on the selection (DelayFilter).
class UpstreamModel {
public:
    struct Instance {
        bool capture = false;
        bool pre_mix = false;
        bool post_mix_installed = true;
        std::string device = std::string("CABLE Input VB-Audio Virtual Cable ") + kStereo;
    };

    UpstreamModel(const ChannelLayout& layout, double rate, Instance instance = Instance())
        : rate_(rate), instance_(std::move(instance)) {
        const int channels = static_cast<int>(layout.channels);
        const int mask = layout.speaker_mask != 0 ? static_cast<int>(layout.speaker_mask)
                                                  : upstream::default_channel_mask(channels);
        names_ = upstream::channel_names(channels, mask);
        names_.resize(layout.channels);   // the model's buffers are the device's channels
    }

    // [channel][frame] in, the device channels out: one file as the whole
    // configuration.
    std::vector<std::vector<double>> run(const std::string& text, std::vector<std::vector<double>> x) {
        return run_config(text, {}, std::move(x));
    }

    // config.txt, with the files its Include lines name, keyed by the name as written.
    std::vector<std::vector<double>> run_config(const std::string& config,
                                                const std::map<std::string, std::string>& files,
                                                std::vector<std::vector<double>> x) {
        x_ = std::move(x);
        frames_ = x_[0].size();
        virtuals_.clear();
        selection_.clear();
        for (size_t c = 0; c < names_.size(); ++c) selection_.push_back(static_cast<long>(c));
        // startOfConfiguration
        device_matches_ = true;
        true_count_ = false_count_ = 0;
        true_counts_.clear();
        stage_matches_ = instance_.capture || !instance_.pre_mix || !instance_.post_mix_installed;
        stage_stack_.clear();
        endif_without_if_ = 0;
        load_file(config, files);
        x_.resize(names_.size());
        return x_;
    }

    // How many times the last run logged "EndIf without If!".
    int endif_without_if() const { return endif_without_if_; }

private:
    static std::string trim(const std::string& s) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    }

    // Whether `expression` holds on this device; only the forms used here.
    bool holds(const std::string& expression) {
        std::istringstream in(expression);
        std::string name, op;
        double value = 0;
        in >> name >> op >> value;
        if (name == "outputChannelCount" && op == "==") return names_.size() == value;
        if (name == "sampleRate" && op == ">=") return rate_ >= value;
        if (name == "sampleRate" && op == "==") return rate_ == value;
        FAIL("an If the model does not know: " << expression);
        return false;
    }

    // IfFilterFactory::createFilter, without the logging.
    void conditional(const std::string& command, const std::string& parameters) {
        const std::string expression = trim(parameters);
        if (command == "If") {
            if (false_count_ == 0) {
                if (holds(expression)) {
                    true_count_++;
                } else {
                    false_count_++;
                    execute_else_ = true;
                }
            } else {
                false_count_++;
            }
        } else if (command == "ElseIf") {
            if (false_count_ == 0) {
                if (true_count_ != 0) {
                    false_count_++;
                    true_count_--;
                }
            } else if (false_count_ == 1 && execute_else_ && holds(expression)) {
                false_count_--;
                true_count_++;
                execute_else_ = false;
            }
        } else if (command == "Else") {
            if (false_count_ == 0) {
                if (true_count_ != 0) {
                    false_count_++;
                    true_count_--;
                }
            } else if (false_count_ == 1 && execute_else_) {
                false_count_--;
                true_count_++;
                execute_else_ = false;
            }
        } else if (command == "EndIf") {
            if (false_count_ == 0) {
                if (true_count_ != 0) true_count_--;
            else ++endif_without_if_;   // upstream logs "EndIf without If!" and carries on
            } else {
                false_count_--;
            }
            if (false_count_ == 0) execute_else_ = false;
        }
    }

    void load_file(const std::string& text, const std::map<std::string, std::string>& files) {
        const std::vector<long> saved_selection = selection_;
        // startOfFile: If, Stage
        true_counts_.push_back(true_count_);
        true_count_ = 0;
        execute_else_ = false;
        false_count_ = 0;
        stage_stack_.push_back(stage_matches_);

        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim(line.substr(0, colon));
            const std::string value = line.substr(colon + 1);

            if (key == "Device") device_matches_ = upstream::match_device(instance_.device, value);
            if (!device_matches_) continue;
            if (key == "If" || key == "ElseIf" || key == "Else" || key == "EndIf") conditional(key, value);
            if (false_count_ > 0) continue;
            if (key == "Include") {
                const std::string name = value.substr(std::min(value.find_first_not_of(" \t"), value.size()));
                const auto file = files.find(name);
                REQUIRE_MESSAGE(file != files.end(), "an Include the test did not provide: " << name);
                load_file(file->second, files);
                continue;
            }
            if (key == "Stage") {
                std::string stage = trim(value);
                for (char& c : stage) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                stage_matches_ = false;
                std::istringstream parts(stage);
                for (std::string part; std::getline(parts, part, ' ');) {
                    if (part == "pre-mix" && !instance_.capture && instance_.pre_mix) stage_matches_ = true;
                    if (part == "post-mix" && !instance_.capture && !instance_.pre_mix) stage_matches_ = true;
                    if (part == "capture" && instance_.capture) stage_matches_ = true;
                }
            }
            if (!stage_matches_) continue;
            command(key, value, line);
        }

        // endOfFile: Device, If, Stage; then the outer file's channels.
        device_matches_ = true;
        false_count_ = 0;
        true_count_ = true_counts_.back();
        true_counts_.pop_back();
        stage_matches_ = stage_stack_.back();
        stage_stack_.pop_back();
        selection_ = saved_selection;
    }

    void command(const std::string& key, const std::string& value, const std::string& line) {
        std::istringstream words(value);
        if (key.empty() || key[0] == '#' || key == "Device" || key == "If" || key == "ElseIf" || key == "Else" ||
            key == "EndIf" || key == "Stage") {
            return;
        }
        if (key == "Channel") {
            // ChannelFilterFactory upper-cases the words; ChannelFilter resolves them.
            std::vector<std::string> all_names = names_;
            all_names.insert(all_names.end(), virtuals_.begin(), virtuals_.end());
            std::vector<bool> selected(all_names.size(), false);
            std::string w;
            while (words >> w) {
                for (char& c : w) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (w == "ALL") {
                    selected.assign(all_names.size(), true);
                } else if (const long n = upstream::channel_index(w, all_names); n >= 0) {
                    selected[n] = true;
                }
            }
            selection_.clear();
            for (size_t c = 0; c < selected.size(); ++c) {
                if (selected[c]) selection_.push_back(static_cast<long>(c));
            }
        } else if (key == "Preamp") {
            std::string v = value;
            std::replace(v.begin(), v.end(), ',', '.');
            double db = 0;
            if (sscanf_s(v.c_str(), " %lf dB", &db) != 1) return;
            const double gain = static_cast<float>(std::pow(10.0, db / 20.0));   // PreampFilter's float gain
            for (long n : selection_) for (double& s : x_[n]) s *= gain;
        } else if (key.find("Filter") == 0) {
            // A line upstream makes no filter of (OFF, a missing Hz) does nothing.
            const std::optional<upstream::BiQuadFilterSpec> spec = upstream::create_filter(key, value);
            if (!spec) return;
            for (long n : selection_) {
                upstream::BiQuad bq = upstream::make_biquad(*spec, rate_);
                for (double& s : x_[n]) s = bq.process(s);
            }
        } else if (key == "Copy") {
            const std::vector<std::vector<double>> input = x_;
            // CopyFilter::initialize resolves every target and source against the
            // channels there were before the line. A target that resolves to none
            // is a channel of that name, one per distinct name in the line, which
            // FilterEngine::addFilters appends.
            const size_t known = x_.size();
            std::string assignment;
            while (words >> assignment) {
                const size_t eq = assignment.find('=');
                const std::string target = assignment.substr(0, eq);
                std::vector<double> out(frames_, 0.0);
                std::istringstream sum(assignment.substr(eq + 1));
                std::string term;
                while (std::getline(sum, term, '+')) {
                    const size_t star = term.find('*');
                    std::string factor_text, channel;
                    if (star != std::string::npos) {
                        factor_text = term.substr(0, star);
                        channel = term.substr(star + 1);
                    } else if (term == "0" || term.find('.') != std::string::npos) {
                        factor_text = term;
                    } else {
                        channel = term;
                    }
                    const double factor = factor_text.empty() ? 1.0 : std::stod(factor_text);
                    const long source = channel.empty() ? -1 : index(channel, known);
                    for (size_t f = 0; f < frames_; ++f) out[f] += source < 0 ? factor : factor * input[source][f];
                }
                long t = index(target, known);
                if (t < 0) {
                    const auto added = virtuals_.begin() + static_cast<long>(known - names_.size());
                    const auto same = std::find(added, virtuals_.end(), target);
                    if (same != virtuals_.end()) {
                        t = static_cast<long>(names_.size() + (same - virtuals_.begin()));
                    } else {
                        virtuals_.push_back(target);
                        x_.emplace_back(frames_, 0.0);
                        t = static_cast<long>(x_.size() - 1);
                    }
                }
                x_[t] = out;
            }
        } else if (key == "Delay") {
            double ms = 0;
            std::string unit;
            words >> ms >> unit;
            REQUIRE(unit == "ms");
            const size_t n = static_cast<size_t>(rate_ * ms / 1000.0 + 0.5);
            for (long c : selection_) {
                std::vector<double>& v = x_[c];
                v.insert(v.begin(), n, 0.0);
                v.resize(frames_);
            }
        } else {
            FAIL("a command the model does not know: " << line);
        }
    }

    // Among the first `count` channels, virtual ones included.
    long index(const std::string& name, size_t count) const {
        std::vector<std::string> all_names = names_;
        all_names.insert(all_names.end(), virtuals_.begin(), virtuals_.end());
        all_names.resize(count);
        return upstream::channel_index(name, all_names);
    }

    double rate_;
    Instance instance_;
    std::vector<std::string> names_;
    std::vector<std::string> virtuals_;
    std::vector<std::vector<double>> x_;
    size_t frames_ = 0;
    std::vector<long> selection_;
    bool device_matches_ = true;
    int true_count_ = 0, false_count_ = 0;
    int endif_without_if_ = 0;
    bool execute_else_ = false;
    std::vector<int> true_counts_;
    bool stage_matches_ = true;
    std::vector<bool> stage_stack_;
};

}  // namespace

TEST_CASE("the text Isotone writes does what the processor does") {
    constexpr double kRate = 48000.0;
    constexpr size_t kFrames = 24000;
    // 5.1 surround: L R C LFE SL SR. Every speaker feature at once, plus bands
    // and a trim, so a difference in any stage or in their order shows up.
    const ChannelLayout layout{6, 0x60F};
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = layout;
    d.state.preamp_db = -4;
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1.4, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::LowShelf, 120, 5, 0.7, WidthMode::Q, 1u << 3));
    d.state.bands.push_back(band(FilterType::HighShelf, 6000, 3, 0.7, WidthMode::Q, (1u << 0) | (1u << 4)));
    d.state.channel_gain_db[1] = -2.5;
    SpeakerSetup& sp = d.state.speakers;
    sp.swap_left_right = true;
    sp.upmix = Upmix::All;
    sp.bass_management = true;
    sp.crossover_hz = 90;
    sp.lfe_lowpass_hz = 110;
    sp.small_speakers = 0x37;   // L R C SL SR
    sp.inverted = 1u << 4;
    sp.muted = 1u << 5;
    sp.delay_ms[0] = 1.2;
    sp.delay_ms[2] = 0.8;
    sp.lip_sync_ms = 3;

    // A different mixture of bass, mids and treble on every channel.
    std::vector<std::vector<double>> input(6, std::vector<double>(kFrames));
    for (uint32_t c = 0; c < 6; ++c) {
        for (size_t f = 0; f < kFrames; ++f) {
            const double t = static_cast<double>(f) / kRate;
            input[c][f] = 0.3 * std::sin(2 * 3.14159265358979 * (45.0 + 7 * c) * t) +
                          0.2 * std::sin(2 * 3.14159265358979 * (900.0 + 110 * c) * t + c) +
                          0.1 * std::sin(2 * 3.14159265358979 * (7000.0 - 300 * c) * t);
        }
    }

    const std::string block = format_device_block(d);
    MESSAGE(block);
    UpstreamModel model(layout, kRate);
    const std::vector<std::vector<double>> expected = model.run(block, input);

    Processor p;
    p.initialize(kRate, 6, 480, 64, layout.speaker_mask);
    p.set_target(d.state);
    p.reset();
    std::vector<std::vector<float>> buf(6, std::vector<float>(480));
    std::vector<float*> ptr(6);
    double worst = 0.0;
    size_t worst_c = 0, worst_f = 0;
    for (size_t pos = 0; pos < kFrames; pos += 480) {
        for (uint32_t c = 0; c < 6; ++c) {
            for (size_t i = 0; i < 480; ++i) buf[c][i] = static_cast<float>(input[c][pos + i]);
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), 480);
        for (uint32_t c = 0; c < 6; ++c) {
            for (size_t i = 0; i < 480; ++i) {
                const double diff = std::abs(buf[c][i] - expected[c][pos + i]);
                if (diff > worst) {
                    worst = diff;
                    worst_c = c;
                    worst_f = pos + i;
                }
            }
        }
    }
    CAPTURE(worst_c);
    CAPTURE(worst_f);
    CHECK(worst < 1e-4);

    // Not vacuous: the stages changed the signal a lot.
    double moved = 0.0;
    for (uint32_t c = 0; c < 6; ++c) {
        for (size_t f = 12000; f < kFrames; ++f) moved = std::max(moved, std::abs(expected[c][f] - input[c][f]));
    }
    CHECK(moved > 0.2);

    // And the settings come back out of the file.
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].warnings.empty());
    CHECK(format_speaker_setup(parsed[0].state.speakers) == format_speaker_setup(sp));
    CHECK(parsed[0].state.bands.size() == 3);
}

TEST_CASE("a band beyond the device's Nyquist is written at the frequency the processor designs") {
    DeviceConfig d = stereo_device();
    d.sample_rate = 44100.0;
    d.state.bands.clear();
    d.state.bands.push_back(band(FilterType::Peaking, 30000, -6, 1, WidthMode::Q));
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed[0].state.bands.size() == 1);
    CHECK(parsed[0].state.bands[0].fc == doctest::Approx(clamp_fc(30000, 44100.0)));
}

TEST_CASE("updating one device keeps every other byte of the file") {
    DeviceConfig a = stereo_device();
    DeviceConfig b = height_device();
    DeviceConfig c = stereo_device();
    c.endpoint_guid = "{22222222-0000-0000-0000-000000000000}";
    const std::string text = update_isotone_file(update_isotone_file(update_isotone_file("", a), b), c);

    const size_t b_at = text.find("Device: {11111111");
    const size_t c_at = text.find("Device: {22222222");
    REQUIRE(b_at != std::string::npos);
    REQUIRE(c_at != std::string::npos);
    const std::string before_b = text.substr(0, b_at);
    const std::string from_c = text.substr(c_at);

    // Braces and case do not make a different device.
    b.endpoint_guid = "11111111-2222-3333-4444-555555555555";
    b.state.bands[0].gain_db = -9;
    const std::string updated = update_isotone_file(text, b);
    CHECK(updated.substr(0, b_at) == before_b);
    CHECK(updated.size() - updated.find("Device: {22222222") == from_c.size());
    CHECK(updated.substr(updated.find("Device: {22222222")) == from_c);
    CHECK(updated.find("Gain -9 dB") != std::string::npos);
    CHECK(update_isotone_file(updated, b) == updated);

    const std::string removed = remove_device(updated, kHeight);
    CHECK(removed == before_b + from_c);
    CHECK(remove_device(removed, kHeight) == removed);
}

// ---------------------------------------------------------------------------
// Committing edits

TEST_CASE("the writer keeps live edits in memory and persists at once") {
    Sandbox s;
    CompatWriter writer(s.dir);
    REQUIRE(writer.load() == ERROR_SUCCESS);
    CHECK(writer.text().empty());

    DeviceConfig d = stereo_device();
    d.sample_rate = 48000.0;
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    CHECK(writer.has_pending());
    CHECK_FALSE(fs::exists(writer.path()));

    d.state.preamp_db = -8;
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    CHECK_FALSE(writer.has_pending());
    CHECK(get(writer.path()) == writer.text());
    CHECK(no_temporary_files(s.dir));

    // A second writer on the same directory picks up what is there.
    CompatWriter again(s.dir);
    REQUIRE(again.load() == ERROR_SUCCESS);
    CHECK(again.text() == writer.text());

    // And each keeps the other's devices: a write never reverts a block the
    // other wrote since this one last read the file.
    DeviceConfig other = height_device();
    other.sample_rate = 48000.0;
    REQUIRE(again.persist(other) == ERROR_SUCCESS);
    d.state.preamp_db = -9;
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    const std::vector<ParsedDevice> both = parse_isotone_file(get(writer.path()), layout_for);
    REQUIRE(both.size() == 2);
    CHECK(both[0].state.preamp_db == -9.0);
    CHECK(both[1].endpoint_guid == kHeight);
}

TEST_CASE("a live edit still pending when the writer goes away is written") {
    Sandbox s;
    DeviceConfig d = stereo_device();
    d.sample_rate = 48000.0;
    {
        CompatWriter writer(s.dir);
        REQUIRE(writer.load() == ERROR_SUCCESS);
        REQUIRE(writer.apply(d) == ERROR_SUCCESS);
        d.state.preamp_db = -11;
        REQUIRE(writer.apply(d) == ERROR_SUCCESS);
        REQUIRE(writer.has_pending());
        REQUIRE_FALSE(fs::exists(s.dir / "Isotone.txt"));
    }
    const std::vector<ParsedDevice> back = parse_isotone_file(get(s.dir / "Isotone.txt"), layout_for);
    REQUIRE(back.size() == 1);
    CHECK(back[0].state.preamp_db == -11.0);
}

// ---------------------------------------------------------------------------
// Defects found by the 2026-09-13 review

namespace {

std::string utf8_of(const fs::path& p) {
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Runs the block for `d` through the upstream model on a device with the layout
// `played`, and `state` through the processor on that layout, returning the
// worst sample difference.
double model_vs_processor(const DeviceConfig& d, const ChannelLayout& played, const EqState& state) {
    constexpr double kRate = 48000.0;
    constexpr size_t kFrames = 24000;
    const ChannelLayout layout = played;
    std::vector<std::vector<double>> input(layout.channels, std::vector<double>(kFrames));
    for (uint32_t c = 0; c < layout.channels; ++c) {
        for (size_t f = 0; f < kFrames; ++f) {
            const double t = static_cast<double>(f) / kRate;
            input[c][f] = 0.4 * std::sin(2 * 3.14159265358979 * (40.0 + 9 * c) * t) +
                          0.2 * std::sin(2 * 3.14159265358979 * (1500.0 + 70 * c) * t);
        }
    }
    UpstreamModel::Instance device;
    device.device = "Test device " + d.endpoint_guid;
    UpstreamModel model(layout, kRate, device);
    const std::vector<std::vector<double>> expected = model.run(format_device_block(d), input);
    Processor p;
    p.initialize(kRate, layout.channels, 480, 64, layout.speaker_mask);
    p.set_target(state);
    p.reset();
    std::vector<std::vector<float>> buf(layout.channels, std::vector<float>(480));
    std::vector<float*> ptr(layout.channels);
    double worst = 0.0;
    for (size_t pos = 0; pos < kFrames; pos += 480) {
        for (uint32_t c = 0; c < layout.channels; ++c) {
            for (size_t i = 0; i < 480; ++i) buf[c][i] = static_cast<float>(input[c][pos + i]);
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), 480);
        for (uint32_t c = 0; c < layout.channels; ++c) {
            for (size_t i = 0; i < 480; ++i) worst = std::max(worst, std::abs(buf[c][i] - expected[c][pos + i]));
        }
    }
    return worst;
}

// The same, on the device's own layout.
double model_vs_processor(const DeviceConfig& d) { return model_vs_processor(d, d.layout, d.state); }

}  // namespace

TEST_CASE("a crossover with three decimals reaches Equalizer APO as written, not 1000 times higher") {
    // Upstream reads "80.125" as Room EQ Wizard's thousands separator. The
    // model parses Filter lines with the same rule, so a crossover written that
    // way would be designed at 80125 Hz and the outputs would disagree.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{6, 0x60F};
    d.state.speakers.bass_management = true;
    d.state.speakers.small_speakers = 0x37;
    d.state.speakers.crossover_hz = 80.125;
    d.state.speakers.lfe_lowpass_hz = 150.25;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("Fc 80.1250 Hz") != std::string::npos);
    CHECK(model_vs_processor(d) < 1e-4);
}

TEST_CASE("mute in Isotone.txt is silence, and reads back as mute") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{6, 0x60F};
    d.state.bands.push_back(band(FilterType::Peaking, 1000, 12, 1, WidthMode::Q));
    d.state.mute = true;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    // Not a -100 dB cut: a gain that underflows upstream's float to zero.
    CHECK(block.find("Preamp: -100 dB\n") == std::string::npos);
    CHECK(block.find("Preamp: -1000 dB\n") != std::string::npos);
    CHECK(model_vs_processor(d) < 1e-6);
    // Silence on whatever layout the device reports when Equalizer APO loads.
    for (const ChannelLayout device : {ChannelLayout{2, 0x3}, ChannelLayout{8, 0x63F}}) {
        std::vector<std::vector<double>> input(device.channels, std::vector<double>(480, 0.5));
        const std::vector<std::vector<double>> out = UpstreamModel(device, 48000.0).run(block, input);
        double loudest = 0.0;
        for (const auto& ch : out) for (double v : ch) loudest = std::max(loudest, std::abs(v));
        CAPTURE(device.channels);
        CHECK(loudest == 0.0);
    }
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].state.mute);
    CHECK(parsed[0].unsupported.empty());
    CHECK(parsed[0].state.bands.size() == 1);
}

TEST_CASE("a layout given with no speaker mask gets the default one, so speaker features are written") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{2, 0};
    d.state.speakers.swap_left_right = true;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("Copy: 1=1*2 2=1*1") != std::string::npos);
}

TEST_CASE("a block written for one layout plays no DC on a device that reports another") {
    // Equalizer APO applies Isotone.txt to the format the device has when it
    // loads, which can differ from the one Isotone wrote for. A Copy source
    // naming a channel the device lacks is added as a constant: full-scale DC.
    const ChannelLayout layouts[] = {{2, 0x3},   {3, 0xB},   {3, 0x7},   {4, 0x33},
                                     {4, 0x107}, {6, 0x60F}, {6, 0x3F},  {8, 0x63F}};
    for (const ChannelLayout& written : layouts) {
        DeviceConfig d;
        d.endpoint_guid = kStereo;
        d.layout = written;
        d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q));
        SpeakerSetup& sp = d.state.speakers;
        sp.swap_left_right = true;
        sp.swap_front_rear = true;
        sp.upmix = Upmix::All;
        sp.bass_management = true;
        sp.small_speakers = 0xFF;
        sp.inverted = 1u << 1;
        sp.muted = 1u << 2;
        sp.delay_ms[0] = 1.0;
        const std::string block = format_device_block(d);
        CAPTURE(block);
        // On its own layout the text is still what the processor does.
        CHECK(model_vs_processor(d) < 1e-4);
        for (const ChannelLayout& device : layouts) {
            CAPTURE(device.channels);
            CAPTURE(device.speaker_mask);
            constexpr size_t kFrames = 9600;
            std::vector<std::vector<double>> input(device.channels, std::vector<double>(kFrames));
            for (uint32_t c = 0; c < device.channels; ++c) {
                for (size_t f = 0; f < kFrames; ++f) {
                    input[c][f] = 0.5 * std::sin(2 * 3.14159265358979 * (500.0 + 100 * c) * f / 48000.0);
                }
            }
            const std::vector<std::vector<double>> out = UpstreamModel(device, 48000.0).run(block, input);
            for (uint32_t c = 0; c < device.channels; ++c) {
                double mean = 0.0;
                for (size_t f = kFrames / 2; f < kFrames; ++f) mean += out[c][f];
                mean /= static_cast<double>(kFrames / 2);
                CAPTURE(c);
                CHECK(std::abs(mean) < 1e-3);
            }
        }
    }
}

TEST_CASE("a block written for 7.1 plays each speaker's values on that speaker on another layout, as IsoAPO does") {
    // L R C LFE RL RR SL SR. SL stands in for RL on 5.1 surround, and RL for SL
    // on 5.1 back and quad, so both land on one channel there.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {8, 0x63F};
    d.sample_rate = 48000.0;
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q, 1u << 6));
    d.state.channel_gain_db[7] = -3.0;
    SpeakerSetup& sp = d.state.speakers;
    sp.delay_ms[6] = 1.0;
    sp.delay_ms[4] = 0.5;
    sp.inverted = (1u << 4) | (1u << 6) | (1u << 7);
    sp.muted = 1u << 2;
    sp.lip_sync_ms = 0.25;
    const std::string block = format_device_block(d);
    MESSAGE(block);
    CHECK(block.find("If: outputChannelCount") == std::string::npos);

    for (const ChannelLayout played :
         {ChannelLayout{8, 0x63F}, ChannelLayout{6, 0x60F}, ChannelLayout{6, 0x3F}, ChannelLayout{4, 0x33},
          ChannelLayout{2, 0x3}, ChannelLayout{3, 0x7}}) {
        CAPTURE(played.channels);
        CAPTURE(played.speaker_mask);
        // IsoAPO on that layout plays the state remap_channels moves there.
        EqState moved = d.state;
        moved.layout_channels = d.layout.channels;
        moved.layout_speaker_mask = d.layout.speaker_mask;
        remap_channels(&moved, played);
        CHECK(model_vs_processor(d, played, moved) < 1e-4);

        // Not vacuous: on stereo only lip sync is left, 12 samples on L and R.
        std::vector<std::vector<double>> input(played.channels, std::vector<double>(4800));
        for (uint32_t c = 0; c < played.channels; ++c) {
            for (size_t f = 0; f < 4800; ++f) input[c][f] = std::sin(0.05 * static_cast<double>(f) + c);
        }
        const std::vector<std::vector<double>> out = UpstreamModel(played, 48000.0).run(block, input);
        if (played.channels == 2) {
            double err = 0.0;
            for (uint32_t c = 0; c < 2; ++c) {
                for (size_t f = 12; f < 4800; ++f) err = std::max(err, std::abs(out[c][f] - input[c][f - 12]));
            }
            CHECK(err < 1e-12);
        }
        if (played.speaker_mask == 0x60F) {
            // SL: RL's 0.5 ms and its own 1 ms add, and inverted once.
            CHECK(moved.speakers.delay_ms[4] == 1.5);
            CHECK(moved.speakers.inverted == ((1u << 4) | (1u << 5)));
            CHECK(*std::max_element(out[2].begin(), out[2].end()) == 0.0);
            CHECK(*std::min_element(out[2].begin(), out[2].end()) == 0.0);
        }
    }
}

TEST_CASE("lip sync and a speaker's delay reach Equalizer APO as the samples the processor rounds their sum to") {
    // 0.01 ms is 0.48 samples at 48 kHz: rounded apart they are no delay, and
    // their sum is one sample.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {2, 0x3};
    d.sample_rate = 48000.0;
    d.state.speakers.lip_sync_ms = 0.01;
    d.state.speakers.delay_ms[0] = 0.01;
    CAPTURE(format_device_block(d));
    CHECK(model_vs_processor(d) < 1e-4);
}

TEST_CASE("a state written for another layout is moved to the device's before it is written") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {6, 0x60F};
    d.sample_rate = 48000.0;
    d.state.layout_channels = 8;
    d.state.layout_speaker_mask = 0x63F;
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q, 1u << 6));   // 7.1's SL
    d.state.speakers.muted = 1u << 7;                                                         // 7.1's SR
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("Channel: SL\n") != std::string::npos);
    CHECK(block.find("# Isotone: layout 6 0x60f\n") != std::string::npos);
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed[0].state.bands.size() == 1);
    CHECK(parsed[0].state.bands[0].channels == 1u << 4);
    CHECK(parsed[0].state.speakers.muted == 1u << 5);
}

TEST_CASE("a block read for another layout moves the speaker setup to that layout's speakers") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {8, 0x63F};
    d.sample_rate = 48000.0;
    SpeakerSetup& sp = d.state.speakers;
    sp.delay_ms[6] = 1.0;        // SL
    sp.inverted = 1u << 7;       // SR
    sp.muted = 1u << 2;          // C
    sp.small_speakers = 1u << 6; // SL
    const std::string text = update_isotone_file("", d);
    CAPTURE(text);
    CHECK(text.find("# Isotone: layout 8 0x63f\n") != std::string::npos);

    const auto on51 = parse_isotone_file(text, [](const std::string&) { return ChannelLayout{6, 0x60F}; });
    REQUIRE(on51.size() == 1);
    CHECK(on51[0].warnings.empty());
    CHECK(on51[0].state.layout_channels == 6);
    CHECK(on51[0].state.speakers.delay_ms[4] == 1.0);
    CHECK(on51[0].state.speakers.delay_ms[6] == 0.0);
    CHECK(on51[0].state.speakers.inverted == 1u << 5);
    CHECK(on51[0].state.speakers.muted == 1u << 2);
    CHECK(on51[0].state.speakers.small_speakers == 1u << 4);

    // On its own layout it reads back as written.
    const auto on71 = parse_isotone_file(text, [&](const std::string&) { return d.layout; });
    REQUIRE(on71.size() == 1);
    CHECK(format_speaker_setup(on71[0].state.speakers) == format_speaker_setup(sp));

    // A layout marker that cannot be read is reported, and the values stay as written.
    std::string bad = text;
    bad.replace(bad.find("# Isotone: layout 8 0x63f"), 25, "# Isotone: layout eight");
    const auto unread = parse_isotone_file(bad, [](const std::string&) { return ChannelLayout{6, 0x60F}; });
    REQUIRE(unread.size() == 1);
    CHECK(unread[0].warnings.size() == 1);
    CHECK(unread[0].state.speakers.delay_ms[6] == 1.0);
}

TEST_CASE("a block read for no layout is read for stereo, speaker setup included") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {8, 0x63F};
    d.sample_rate = 48000.0;
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q, 1u << 1));   // R
    d.state.speakers.inverted = (1u << 1) | (1u << 7);                                        // R, SR
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [](const std::string&) { return ChannelLayout{}; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].state.layout_channels == 2);
    CHECK(parsed[0].state.layout_speaker_mask == 0x3u);
    REQUIRE(parsed[0].state.bands.size() == 1);
    CHECK(parsed[0].state.bands[0].channels == 1u << 1);
    CHECK(parsed[0].state.speakers.inverted == 1u << 1);
}

TEST_CASE("a channel count past what a stream can carry is written as the most it can") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {1000000, 0x3};
    d.sample_rate = 48000.0;
    d.state.speakers.swap_left_right = true;
    d.state.speakers.lip_sync_ms = 1.0;
    const std::string block = format_device_block(d);
    CHECK(block.find("If: outputChannelCount == 65535\n") != std::string::npos);
}

TEST_CASE("bands are guarded by the lowest rate they are stable at") {
    // A band written at 22800 Hz for a 48 kHz device is above Nyquist at
    // 44.1 kHz, where upstream designs it unstable.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {2, 0x3};
    d.sample_rate = 48000.0;
    d.state.channel_gain_db[0] = -2.0;
    d.state.bands.push_back(band(FilterType::Peaking, 23000, -6, 1, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q));
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("If: sampleRate >= 48000\n") != std::string::npos);

    std::vector<std::vector<double>> input(2, std::vector<double>(4800));
    for (size_t f = 0; f < 4800; ++f) input[0][f] = input[1][f] = std::sin(2 * 3.14159265358979 * 1000.0 * f / 44100.0);
    // At 44.1 kHz the bands do nothing, and the trim, which has no frequency, stays.
    const std::vector<std::vector<double>> low = UpstreamModel(d.layout, 44100.0).run(block, input);
    double err = 0.0;
    for (size_t f = 0; f < 4800; ++f) {
        err = std::max(err, std::abs(low[1][f] - input[1][f]));
        err = std::max(err, std::abs(low[0][f] - input[0][f] * static_cast<float>(std::pow(10.0, -2.0 / 20.0))));
    }
    CHECK(err < 1e-6);
    // At the rate it was written for it is the processor's curve.
    CHECK(model_vs_processor(d) < 1e-4);

    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].warnings.empty());
    CHECK(parsed[0].unsupported.empty());
    CHECK(parsed[0].state.bands.size() == 2);

    // Low bands need no more than a low rate.
    d.state.bands.erase(d.state.bands.begin());
    CHECK(format_device_block(d).find("If: sampleRate >= 2106\n") != std::string::npos);
}

TEST_CASE("bypass turns off the preamp and bands, and nothing else, in Equalizer APO too") {
    DeviceConfig d = stereo_device();
    d.state.bypass = true;
    d.state.mute = false;
    CHECK(model_vs_processor(d) < 1e-4);
    DeviceConfig on = d;
    on.state.bypass = false;
    CHECK(model_vs_processor(on) < 1e-4);
}

TEST_CASE("a second block for the same device is removed on update and reported on read") {
    DeviceConfig a = stereo_device();
    DeviceConfig other = height_device();
    const std::string one = format_device_block(a);
    a.state.preamp_db = -9;
    const std::string stale = format_device_block(a);
    const std::string text = one + "\n" + format_device_block(other) + "\n" + stale;
    const std::vector<ParsedDevice> read = parse_isotone_file(text, layout_for);
    REQUIRE(read.size() == 3);
    CHECK(read[0].warnings.empty());
    CHECK_FALSE(read[2].warnings.empty());

    a.state.preamp_db = -1;
    const std::string updated = update_isotone_file(text, a);
    const std::vector<ParsedDevice> back = parse_isotone_file(updated, layout_for);
    REQUIRE(back.size() == 2);
    CHECK(back[0].state.preamp_db == -1.0);
    CHECK(back[1].endpoint_guid == kHeight);
    CHECK(remove_device(text, kStereo) == format_device_block(other) + "\n");
}

TEST_CASE("a full device ID names the endpoint by its GUID") {
    DeviceConfig d = stereo_device();
    d.endpoint_guid = "{0.0.0.00000000}.{798436D2-8C71-4834-9248-00CCBAACA00A}";
    const std::string text = update_isotone_file("", d);
    CHECK(text.find("Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\n") != std::string::npos);
    DeviceConfig bare = stereo_device();
    bare.state.preamp_db = -2;
    const std::vector<ParsedDevice> back = parse_isotone_file(update_isotone_file(text, bare), layout_for);
    REQUIRE(back.size() == 1);
    CHECK(back[0].state.preamp_db == -2.0);
}

TEST_CASE("a routing section that lost its end marker costs the section, not the curve") {
    const std::string text = "Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n# Isotone: routing\n"
                             "If: outputChannelCount == 2\nCopy: 1=1*2 2=1*1\nEndIf:\nChannel: all\n"
                             "Preamp: -3 dB\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n";
    const std::vector<ParsedDevice> back = parse_isotone_file(text, layout_for);
    REQUIRE(back.size() == 1);
    CHECK_FALSE(back[0].warnings.empty());
    CHECK(back[0].state.preamp_db == -3.0);
    CHECK(back[0].state.bands.size() == 1);
}

TEST_CASE("an Include of Isotone.txt by absolute path counts as attached") {
    Sandbox s;
    const std::string absolute = "Include: " + utf8_of(s.dir / "Isotone.txt") + "\r\n";
    put(s.dir / "config.txt", std::string(kOwnerConfig) + absolute);
    CHECK(inspect_config(s.dir).isotone_included);
    const AttachResult r = attach_include(s.dir);
    CHECK(r.error == ERROR_SUCCESS);
    CHECK_FALSE(r.appended);
    CHECK(get(s.dir / "config.txt") == std::string(kOwnerConfig) + absolute);

    // An Isotone.txt somewhere else is a different file.
    Sandbox other;
    put(s.dir / "config.txt", std::string(kOwnerConfig) + "Include: " + utf8_of(other.dir / "Isotone.txt") + "\r\n");
    CHECK_FALSE(inspect_config(s.dir).isotone_included);
}

TEST_CASE("attach and detach refuse a config.txt that is a hard link to another file") {
    // A link planted in a sandbox would otherwise carry the append or the
    // truncation to the file it points at, such as the live config.txt.
    Sandbox s;
    Sandbox elsewhere;
    const fs::path target = elsewhere.dir / "config.txt";
    put(target, kOwnerConfig);
    REQUIRE(CreateHardLinkW((s.dir / "config.txt").c_str(), target.c_str(), nullptr));
    CHECK(attach_include(s.dir).error == ERROR_CANT_ACCESS_FILE);
    CHECK(get(target) == kOwnerConfig);

    put(target, std::string(kOwnerConfig) + block("\r\n"));   // as if attached
    bool removed = false;
    CHECK(detach_include(s.dir, &removed) == ERROR_CANT_ACCESS_FILE);
    CHECK_FALSE(removed);
    CHECK(get(target) == std::string(kOwnerConfig) + block("\r\n"));
}

TEST_CASE("an atomic write does not write through a .tmp name planted as a hard link") {
    Sandbox s;
    const fs::path victim = s.dir / "victim.txt";
    put(victim, "keep me");
    // The name this thread's write uses, in the temporary directory and, when
    // that is on another volume, next to the target.
    const std::wstring tmp_name = L"Isotone.txt." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                  std::to_wstring(GetCurrentThreadId()) + L".tmp";
    fs::create_directories(default_temp_dir());
    REQUIRE(CreateHardLinkW((default_temp_dir() / tmp_name).c_str(), victim.c_str(), nullptr));
    REQUIRE(write_file_atomically(s.dir / "Isotone.txt", "new content") == ERROR_SUCCESS);
    CHECK(get(victim) == "keep me");
    CHECK(get(s.dir / "Isotone.txt") == "new content");

    REQUIRE(CreateHardLinkW((s.dir / tmp_name).c_str(), victim.c_str(), nullptr));
    REQUIRE(write_file_atomically(s.dir / "Isotone.txt", "newer content", 200, unmounted_drive_dir()) == ERROR_SUCCESS);
    CHECK(get(victim) == "keep me");
    CHECK(get(s.dir / "Isotone.txt") == "newer content");
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("detach waits for a writer holding config.txt, then cuts the file it checked") {
    Sandbox s;
    const fs::path config = s.dir / "config.txt";
    put(config, kOwnerConfig);
    REQUIRE(attach_include(s.dir).appended);
    // Another program (Peace, an editor) has the file open for writing.
    HANDLE held = CreateFileW(config.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CloseHandle(held);
    });
    bool removed = false;
    const DWORD e = detach_include(s.dir, &removed);
    release.join();
    CHECK(e == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(config) == kOwnerConfig);
}

// ---------------------------------------------------------------------------
// Defects found by the 2026-09-13 compat review

namespace {

constexpr char kOther[] = "{22222222-0000-0000-0000-000000000000}";

DeviceConfig preamp_device(const char* guid, double preamp_db) {
    DeviceConfig d;
    d.endpoint_guid = guid;
    d.layout = {2, 0x3};
    d.sample_rate = 48000.0;
    d.state.preamp_db = preamp_db;
    return d;
}

// The preamp Isotone.txt holds for `guid`, or NaN when it has no block for it.
double preamp_in(const std::string& text, const std::string& guid) {
    for (const ParsedDevice& d : parse_isotone_file(text, [](const std::string&) { return ChannelLayout{2, 0x3}; })) {
        if (d.endpoint_guid == guid) return d.state.preamp_db;
    }
    return std::nan("");
}

// The gain in dB a configuration applies to a stereo DC input in one instance.
double model_gain_db(const std::string& config, const std::string& isotone_txt, const UpstreamModel::Instance& instance,
                     double rate = 48000.0) {
    std::vector<std::vector<double>> input(2, std::vector<double>(64, 0.5));
    const auto out = UpstreamModel({2, 0x3}, rate, instance)
                         .run_config(config, {{"Isotone.txt", isotone_txt}}, input);
    return 20.0 * std::log10(out[0][10] / 0.5);
}

UpstreamModel::Instance post_mix() { return UpstreamModel::Instance(); }
UpstreamModel::Instance pre_mix() {
    UpstreamModel::Instance i;
    i.pre_mix = true;
    return i;
}
UpstreamModel::Instance capture() {
    UpstreamModel::Instance i;
    i.capture = true;
    return i;
}

struct FileId {
    DWORD volume = 0, high = 0, low = 0;
    bool operator==(const FileId& o) const { return volume == o.volume && high == o.high && low == o.low; }
};
FileId file_id(const fs::path& p) {
    FileId id;
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info)) {
        id = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return id;
}

}  // namespace

TEST_CASE("the rate guard turns off the bands and keeps the preamp") {
    // Auto preamp makes room for the bands and for routing, which the channel
    // count guard keeps at every rate. At a rate below the bands' guard the
    // processor still applies the preamp; so must Equalizer APO.
    DeviceConfig d = preamp_device(kStereo, -21.0);
    d.sample_rate = 96000.0;
    d.state.bands.push_back(band(FilterType::Peaking, 30000, -6, 1, WidthMode::Q));
    const std::string block = format_device_block(d);
    CAPTURE(block);
    REQUIRE(block.find("If: sampleRate >= ") != std::string::npos);

    std::vector<std::vector<double>> input(2, std::vector<double>(4800));
    for (size_t f = 0; f < 4800; ++f) input[0][f] = input[1][f] = std::sin(2 * 3.14159265358979 * 1000.0 * f / 48000.0);
    const auto at_48k = UpstreamModel(d.layout, 48000.0).run(block, input);
    const double gain = static_cast<float>(std::pow(10.0, -21.0 / 20.0));
    double err = 0.0;
    for (uint32_t c = 0; c < 2; ++c) {
        for (size_t f = 0; f < 4800; ++f) err = std::max(err, std::abs(at_48k[c][f] - input[c][f] * gain));
    }
    CHECK(err < 1e-6);

    // Bypass still turns the preamp off with the bands.
    d.state.bypass = true;
    const auto bypassed = UpstreamModel(d.layout, 48000.0).run(format_device_block(d), input);
    err = 0.0;
    for (uint32_t c = 0; c < 2; ++c) {
        for (size_t f = 0; f < 4800; ++f) err = std::max(err, std::abs(bypassed[c][f] - input[c][f]));
    }
    CHECK(err == 0.0);
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].warnings.empty());
    CHECK(parsed[0].state.bypass);
    CHECK(parsed[0].state.preamp_db == -21.0);
    CHECK(parsed[0].state.bands.size() == 1);
}

TEST_CASE("an Include under a Stage or an open If is not attached, and attach appends where it applies") {
    Sandbox s;
    const std::string isotone_txt = update_isotone_file("", preamp_device(kStereo, -6.0));
    const auto config = [&] { return get(s.dir / "config.txt"); };

    SUBCASE("an Include under Stage: capture is not attached for render devices, and attach refuses") {
        const std::string text = "Stage: capture\r\nInclude: Isotone.txt\r\n";
        put(s.dir / "config.txt", text);
        CHECK(model_gain_db(text, isotone_txt, post_mix()) == doctest::Approx(0.0));
        CHECK(model_gain_db(text, isotone_txt, capture()) == doctest::Approx(-6.0).epsilon(1e-6));
        const ConfigInspection i = inspect_config(s.dir);
        CHECK_FALSE(i.isotone_included);
        CHECK(i.isotone_included_conditionally);
        // Appending would include it twice for capture devices.
        const AttachResult r = attach_include(s.dir);
        CHECK(r.error == ERROR_ALREADY_EXISTS);
        CHECK_FALSE(r.appended);
        CHECK(config() == text);
    }

    SUBCASE("an Include under a Stage no instance matches is no include at all") {
        const std::string text = "Stage: nowhere\r\nInclude: Isotone.txt\r\n";
        put(s.dir / "config.txt", text);
        const ConfigInspection i = inspect_config(s.dir);
        CHECK_FALSE(i.isotone_included);
        CHECK_FALSE(i.isotone_included_conditionally);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + block("\r\n", {"Stage: post-mix capture"}));
        for (const auto& instance : {post_mix(), capture()}) {
            CHECK(model_gain_db(config(), isotone_txt, instance) == doctest::Approx(-6.0).epsilon(1e-6));
        }
        CHECK(model_gain_db(config(), isotone_txt, pre_mix()) == doctest::Approx(0.0));
        CHECK(inspect_config(s.dir).isotone_included);
    }

    SUBCASE("a Stage line left at the end of config.txt is undone before the Include") {
        const std::string text = "Stage: pre-mix\r\nPreamp: -3 dB\r\n";
        put(s.dir / "config.txt", text);
        // Appended as before, the Include would apply in pre-mix only.
        CHECK(model_gain_db(text + block("\r\n"), isotone_txt, post_mix()) == doctest::Approx(0.0));
        const AttachResult r = attach_include(s.dir);
        REQUIRE(r.error == ERROR_SUCCESS);
        REQUIRE(r.appended);
        CHECK(r.before.stage_changed_at_end);
        CHECK(config() == text + block("\r\n", {"Stage: post-mix capture"}));
        CHECK(model_gain_db(config(), isotone_txt, post_mix()) == doctest::Approx(-6.0).epsilon(1e-6));
        CHECK(model_gain_db(config(), isotone_txt, capture()) == doctest::Approx(-6.0).epsilon(1e-6));
        // Not a second time in the pre-mix instance: only the user's own line.
        CHECK(model_gain_db(config(), isotone_txt, pre_mix()) == doctest::Approx(-3.0).epsilon(1e-6));
        const ConfigInspection after = inspect_config(s.dir);
        CHECK(after.isotone_included);
        CHECK(after.attached_by_isotone);
        CHECK_FALSE(attach_include(s.dir).appended);
        bool removed = false;
        REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
        CHECK(removed);
        CHECK(config() == text);
    }

    SUBCASE("Ifs left open at the end of config.txt are closed before the Include") {
        const std::string text = "If: sampleRate == 44100\r\nPreamp: -3 dB\r\nIf: sampleRate == 44100\r\n"
                                 "EndIf:\r\nIf: outputChannelCount == 2\r\n";
        put(s.dir / "config.txt", text);
        CHECK(model_gain_db(text + block("\r\n"), isotone_txt, post_mix()) == doctest::Approx(0.0));
        CHECK(inspect_config(s.dir).open_ifs == 2);
        const AttachResult r = attach_include(s.dir);
        REQUIRE(r.appended);
        CHECK(config() == text + block("\r\n", {"EndIf:", "EndIf:"}));
        CHECK(model_gain_db(config(), isotone_txt, post_mix()) == doctest::Approx(-6.0).epsilon(1e-6));
        CHECK(model_gain_db(config(), isotone_txt, post_mix(), 44100.0) == doctest::Approx(-9.0).epsilon(1e-6));
        CHECK(inspect_config(s.dir).isotone_included);
        bool removed = false;
        REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
        CHECK(removed);
        CHECK(config() == text);
    }

    SUBCASE("a Stage line only some devices reach makes a later Include conditional") {
        const std::string text = "Device: Speakers\r\nStage: capture\r\nDevice: all\r\nInclude: Isotone.txt\r\n";
        put(s.dir / "config.txt", text);
        const ConfigInspection i = inspect_config(s.dir);
        CHECK_FALSE(i.isotone_included);
        CHECK(i.isotone_included_conditionally);
        CHECK(attach_include(s.dir).error == ERROR_ALREADY_EXISTS);
    }

    SUBCASE("a Stage line that reaches what no Stage line would, and closed Ifs, get the three-line block") {
        const std::string text = "Stage: post-mix capture\r\nIf: sampleRate == 44100\r\nEndIf:\r\n";
        put(s.dir / "config.txt", text);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + block("\r\n"));
    }
}

TEST_CASE("attach adds no blank line, and a line break only after a last line that has none") {
    Sandbox s;
    const std::string isotone_txt = update_isotone_file("", preamp_device(kStereo, -6.0));
    struct Case {
        const char* original;
        std::string appended;
        double gain_db;   // the file's own preamps, then Isotone.txt's
    };
    const Case cases[] = {
        {"Preamp: -3 dB\r\n", block("\r\n"), -9.0},
        {"Preamp: -3 dB\n", block("\n"), -9.0},
        {"Preamp: -3 dB\r\n\r\n", block("\r\n"), -9.0},
        {"Preamp: -1 dB\r\nPreamp: -3 dB", block("\r\n", {}, true), -10.0},
        {"Preamp: -1 dB\nPreamp: -3 dB", block("\n", {}, true), -10.0},
        {"Preamp: -3 dB", block("\r\n", {}, true), -9.0},
        {"", block("\r\n"), -6.0},
    };
    for (const Case& c : cases) {
        const std::string original = c.original;
        CAPTURE(original);
        put(s.dir / "config.txt", original);
        REQUIRE(attach_include(s.dir).appended);
        const std::string attached = get(s.dir / "config.txt");
        CHECK(attached == original + c.appended);
        CHECK(model_gain_db(attached, isotone_txt, post_mix()) == doctest::Approx(c.gain_db).epsilon(1e-6));
        const ConfigInspection after = inspect_config(s.dir);
        CHECK(after.isotone_included);
        CHECK(after.attached_by_isotone);
        CHECK_FALSE(attach_include(s.dir).appended);
        bool removed = false;
        REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
        CHECK(removed);
        CHECK(get(s.dir / "config.txt") == original);
    }
}

namespace {

// Device lines that match CABLE Input, by its GUID, and not the other device.
constexpr char kCablePattern[] = "{798436d2-8c71-4834-9248-00ccbaaca00a}";

UpstreamModel::Instance on_other_device(UpstreamModel::Instance i) {
    i.device = std::string("Speakers Realtek High Definition Audio ") + kOther;
    return i;
}

struct ModelRun {
    double gain_db;
    int endif_without_if;
};

ModelRun model_run(const std::string& config, const std::string& isotone_txt, const UpstreamModel::Instance& instance,
                   double rate) {
    std::vector<std::vector<double>> input(2, std::vector<double>(64, 0.5));
    UpstreamModel model({2, 0x3}, rate, instance);
    const auto out = model.run_config(config, {{"Isotone.txt", isotone_txt}}, input);
    return {20.0 * std::log10(out[0][10] / 0.5), model.endif_without_if()};
}

}  // namespace

TEST_CASE("attach closes each If and undoes each Stage only for the devices a Device line let see it") {
    // Upstream's DeviceFilterFactory skips every other line, If, EndIf and
    // Stage included, on a device its pattern does not match.
    Sandbox s;
    const std::string isotone_txt =
        update_isotone_file(update_isotone_file("", preamp_device(kStereo, -6.0)), preamp_device(kOther, -6.0));
    const auto config = [&] { return get(s.dir / "config.txt"); };
    const std::string cable = std::string("Device: ") + kCablePattern;
    UpstreamModel::Instance pre_mix_alone = pre_mix();
    pre_mix_alone.post_mix_installed = false;

    // Isotone.txt applies once for every device in every instance attach is for,
    // and the block logs no EndIf without If that the file did not already.
    const auto check_everywhere = [&](const std::string& text, bool stage_on_cable, bool stage_on_other) {
        const std::string attached = config();
        for (double rate : {48000.0, 44100.0}) {
            for (bool other : {false, true}) {
                struct Expected {
                    const char* name;
                    UpstreamModel::Instance instance;
                    double gain_db;
                };
                std::vector<Expected> expected = {
                    {"post-mix", post_mix(), -6.0}, {"capture", capture(), -6.0}, {"pre-mix", pre_mix(), 0.0}};
                // A device that saw a Stage line is reset to post-mix capture,
                // which a pre-mix instance with no post-mix installed does not match.
                if (!(other ? stage_on_other : stage_on_cable)) expected.push_back({"pre-mix alone", pre_mix_alone, -6.0});
                for (Expected e : expected) {
                    if (other) e.instance = on_other_device(e.instance);
                    CAPTURE(rate);
                    CAPTURE(other);
                    CAPTURE(e.name);
                    const ModelRun with = model_run(attached, isotone_txt, e.instance, rate);
                    const ModelRun without = model_run(text, "", e.instance, rate);
                    CHECK(with.gain_db == doctest::Approx(e.gain_db).epsilon(1e-6));
                    CHECK(with.endif_without_if == without.endif_without_if);
                }
            }
        }
        const ConfigInspection after = inspect_config(s.dir);
        CHECK(after.isotone_included);
        CHECK(after.attached_by_isotone);
        CHECK_FALSE(attach_include(s.dir).appended);
        bool removed = false;
        REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
        CHECK(removed);
        CHECK(config() == text);
    };

    SUBCASE("an If left open under a Device line") {
        const std::string text = cable + "\r\nIf: sampleRate == 44100\r\n";
        put(s.dir / "config.txt", text);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + attach_text("\r\n", {cable, "EndIf:", "Device: all"}));
        check_everywhere(text, false, false);
    }

    SUBCASE("a Stage line under a Device line") {
        const std::string text = cable + "\r\nStage: capture\r\n";
        put(s.dir / "config.txt", text);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + attach_text("\r\n", {cable, "Stage: post-mix capture", "Device: all"}));
        check_everywhere(text, true, false);
    }

    SUBCASE("Ifs and Stage lines under several Device lines") {
        const std::string text = "If: sampleRate == 44100\r\n" + cable + "\r\nIf: sampleRate == 44100\r\nStage: pre-mix\r\n"
                                 "Device: Speakers\r\nIf: sampleRate == 48000\r\nStage: capture\r\n";
        put(s.dir / "config.txt", text);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + attach_text("\r\n", {"Device: all", "EndIf:", cable, "EndIf:", "Device: Speakers", "EndIf:",
                                                      cable, "Stage: post-mix capture", "Device: Speakers",
                                                      "Stage: post-mix capture", "Device: all"}));
        check_everywhere(text, true, true);
    }

    SUBCASE("an If opened under a Device line and closed under Device: all needs no EndIf") {
        const std::string text = cable + "\r\nIf: sampleRate == 44100\r\nDevice: all\r\nEndIf:\r\n";
        put(s.dir / "config.txt", text);
        REQUIRE(attach_include(s.dir).appended);
        CHECK(config() == text + block("\r\n"));
        check_everywhere(text, false, false);
    }
}

TEST_CASE("a writer waiting for the lock gets it from one that takes it again at once") {
    // A writer persisting back to back leaves the lock free only between
    // releasing it and taking it again. Polling for it missed those gaps for a
    // whole second on a CI runner, and the waiting write failed. A waiter gets
    // it at the first release.
    Sandbox s;
    const fs::path path = s.dir / "Isotone.txt.lock";
    std::atomic<bool> done{false};
    std::atomic<int> held{0};
    std::thread busy([&] {
        while (!done) {
            DirectoryLock lock;
            if (lock.acquire(path, 5000) != ERROR_SUCCESS) return;
            held++;
            Sleep(200);
        }
    });
    while (held == 0) Sleep(1);
    int failures = 0;
    DWORD first_error = ERROR_SUCCESS;
    ULONGLONG longest = 0;
    for (int k = 0; k < 10; ++k) {
        const ULONGLONG start = GetTickCount64();
        DirectoryLock lock;
        const DWORD e = lock.acquire(path, 1000);
        longest = std::max(longest, GetTickCount64() - start);
        if (e != ERROR_SUCCESS && failures++ == 0) first_error = e;
    }
    done = true;
    busy.join();
    CAPTURE(first_error);
    CAPTURE(longest);
    CHECK(failures == 0);
    CHECK(longest < 350);   // one hold of 200 ms, and time to be scheduled
}

TEST_CASE("the writers' lock is released when its holder goes") {
    Sandbox s;
    const fs::path path = s.dir / "Isotone.txt.lock";
    {
        DirectoryLock first;
        REQUIRE(first.acquire(path, 1000) == ERROR_SUCCESS);
        DirectoryLock second;
        CHECK(second.acquire(path, 100) == ERROR_SHARING_VIOLATION);
    }
    DirectoryLock third;
    CHECK(third.acquire(path, 0) == ERROR_SUCCESS);
}

// Run as a child by the next test: holds the lock named in the environment
// until it is killed. Does nothing in a normal run.
TEST_CASE("(helper) hold the writers' lock until killed") {
    wchar_t path[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ISOTONE_TEST_HOLD_LOCK", path, MAX_PATH) == 0) return;
    DirectoryLock lock;
    if (lock.acquire(path, 1000) != ERROR_SUCCESS) return;
    std::printf("held\n");
    std::fflush(stdout);
    Sleep(INFINITE);
}

TEST_CASE("the writers' lock is released when the process holding it is killed") {
    Sandbox s;
    const fs::path path = s.dir / "Isotone.txt.lock";
    wchar_t exe[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"-tc=(helper) hold the writers' lock until killed\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    REQUIRE(CreatePipe(&read_end, &write_end, &sa, 0));
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"ISOTONE_TEST_HOLD_LOCK", path.c_str());
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    SetEnvironmentVariableW(L"ISOTONE_TEST_HOLD_LOCK", nullptr);
    CloseHandle(write_end);
    REQUIRE(started);
    std::string out;
    char buf[256];
    DWORD got = 0;
    while (out.find("held") == std::string::npos && ReadFile(read_end, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CAPTURE(out);
    {
        DirectoryLock while_held;
        CHECK(while_held.acquire(path, 100) == ERROR_SHARING_VIOLATION);
    }
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_end);
    DirectoryLock after;
    CHECK(after.acquire(path, 2000) == ERROR_SUCCESS);
}

TEST_CASE("two writers in two threads never lose each other's blocks") {
    // Each writer re-reads the file after every persist: its own block must be
    // the one it just wrote, whatever the other did in between.
    Sandbox s;
    constexpr int kRounds = 300;
    struct Result {
        int failures = 0, lost = 0;
        DWORD first_error = ERROR_SUCCESS;
    };
    const auto writer = [&](const char* guid, Result* result) {
        CompatWriter w(s.dir);
        if (const DWORD e = w.load(); e != ERROR_SUCCESS) {
            result->failures++;
            result->first_error = e;
            return;
        }
        for (int round = 1; round <= kRounds; ++round) {
            const double db = -0.01 * round;
            if (const DWORD e = w.persist(preamp_device(guid, db)); e != ERROR_SUCCESS) {
                if (result->failures++ == 0) result->first_error = e;
                continue;
            }
            std::string text;
            if (read_file_bytes(s.dir / "Isotone.txt", &text) != ERROR_SUCCESS ||
                !(std::abs(preamp_in(text, guid) - db) < 1e-9)) {
                result->lost++;
            }
        }
    };
    Result a, b;
    std::thread ta(writer, kStereo, &a);
    std::thread tb(writer, kOther, &b);
    ta.join();
    tb.join();
    CAPTURE(a.first_error);
    CAPTURE(b.first_error);
    CHECK(a.failures == 0);
    CHECK(b.failures == 0);
    CHECK(a.lost == 0);
    CHECK(b.lost == 0);
    const std::string text = get(s.dir / "Isotone.txt");
    CHECK(preamp_in(text, kStereo) == doctest::Approx(-0.01 * kRounds));
    CHECK(preamp_in(text, kOther) == doctest::Approx(-0.01 * kRounds));
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a writer in this process and isotone-compat apply in others never lose each other's blocks") {
    Sandbox s;
    Sandbox inputs;
    std::atomic<bool> done{false};
    std::atomic<int> rounds{0}, failures{0}, lost{0};
    std::thread in_process([&] {
        CompatWriter w(s.dir);
        if (w.load() != ERROR_SUCCESS) {
            failures++;
            return;
        }
        for (int round = 1; !done; ++round) {
            const double db = -0.01 * round;
            if (w.persist(preamp_device(kStereo, db)) != ERROR_SUCCESS) {
                failures++;
                continue;
            }
            std::string text;
            if (read_file_bytes(s.dir / "Isotone.txt", &text) != ERROR_SUCCESS ||
                !(std::abs(preamp_in(text, kStereo) - db) < 1e-9)) {
                lost++;
            }
            rounds = round;
        }
    });
    int cli_failures = 0, cli_lost = 0;
    std::string first_output;
    constexpr int kRuns = 25;
    for (int k = 1; k <= kRuns; ++k) {
        const fs::path in = inputs.dir / ("in-" + std::to_string(k) + ".txt");
        put(in, "Preamp: -" + std::to_string(k) + " dB\n");
        const CliResult r = run_cli({L"apply", L"--root", s.dir.wstring(), L"--device", L"{22222222-0000-0000-0000-000000000000}",
                                     L"--channels", L"2", L"--rate", L"48000", in.wstring()});
        if (r.exit_code != 0 || r.out.find("\"ok\":true") == std::string::npos) {
            if (cli_failures++ == 0) first_output = r.out;
            continue;
        }
        std::string text;
        if (read_file_bytes(s.dir / "Isotone.txt", &text) != ERROR_SUCCESS || preamp_in(text, kOther) != -k) {
            cli_lost++;
        }
    }
    done = true;
    in_process.join();
    CAPTURE(first_output);
    CHECK(cli_failures == 0);
    CHECK(cli_lost == 0);
    CHECK(failures == 0);
    CHECK(lost == 0);
    CHECK(rounds > 0);
    CHECK(preamp_in(get(s.dir / "Isotone.txt"), kOther) == -kRuns);
    CHECK(no_temporary_files(s.dir));
}

TEST_CASE("a write the file no longer holds is written again, whoever changed the file") {
    Sandbox s;
    CompatWriter ui(s.dir);
    REQUIRE(ui.load() == ERROR_SUCCESS);
    const DeviceConfig x = preamp_device(kStereo, -3.0);
    REQUIRE(ui.persist(x) == ERROR_SUCCESS);

    // isotone-compat apply, another writer, sets -9 dB.
    {
        CompatWriter cli(s.dir);
        REQUIRE(cli.load() == ERROR_SUCCESS);
        REQUIRE(cli.persist(preamp_device(kStereo, -9.0)) == ERROR_SUCCESS);
    }
    REQUIRE(preamp_in(get(ui.path()), kStereo) == -9.0);
    REQUIRE(ui.persist(x) == ERROR_SUCCESS);
    CHECK(preamp_in(get(ui.path()), kStereo) == -3.0);

    // The file deleted.
    REQUIRE(DeleteFileW(ui.path().c_str()));
    REQUIRE(ui.persist(x) == ERROR_SUCCESS);
    CHECK(preamp_in(get(ui.path()), kStereo) == -3.0);

    // And a file that already holds it is not replaced, which would make
    // Equalizer APO reload for nothing.
    const FileId before = file_id(ui.path());
    REQUIRE(ui.persist(x) == ERROR_SUCCESS);
    CHECK(file_id(ui.path()) == before);
}

TEST_CASE("the owner of a writer can learn that the last live edit did not reach the file") {
    Sandbox s;
    DeviceConfig d = preamp_device(kStereo, -3.0);
    CompatWriter writer(s.dir);
    REQUIRE(writer.load() == ERROR_SUCCESS);
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    d.state.preamp_db = -11.0;
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    REQUIRE(writer.has_pending());

    // Equalizer APO (or a scanner) holds the file for longer than a write waits.
    const auto hold = [&] {
        HANDLE h = CreateFileW(writer.path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        return h;
    };
    HANDLE held = hold();
    const DWORD e = writer.flush();
    CloseHandle(held);
    CHECK(e != ERROR_SUCCESS);
    CHECK(writer.has_pending());
    CHECK(writer.flush() == ERROR_SUCCESS);
    CHECK_FALSE(writer.has_pending());
    CHECK(preamp_in(get(writer.path()), kStereo) == -11.0);

    // A persist that fails stays pending too.
    held = hold();
    d.state.preamp_db = -12.0;
    const DWORD persisted = writer.persist(d);
    CloseHandle(held);
    CHECK(persisted != ERROR_SUCCESS);
    CHECK(writer.has_pending());
    CHECK(writer.flush() == ERROR_SUCCESS);
    CHECK_FALSE(writer.has_pending());
    CHECK(preamp_in(get(writer.path()), kStereo) == -12.0);
}

namespace {

uint64_t last_write(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    REQUIRE(GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data));
    return (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
}

}  // namespace

TEST_CASE("a live edit changes nothing on disk until it is committed, then is written once") {
    // Every write makes Equalizer APO rebuild every filter chain from rest:
    // measured, a delayed channel goes silent and a bass band swells each time.
    Sandbox s;
    CompatWriter writer(s.dir);
    REQUIRE(writer.load() == ERROR_SUCCESS);
    DeviceConfig d = preamp_device(kStereo, -1.0);
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);   // creates Isotone.txt and the lock

    // What one write looks like in the directory, for comparison.
    std::vector<DirectoryWatch::Event> one_write;
    {
        DirectoryWatch watch(s.dir);
        d.state.preamp_db = -2.0;
        REQUIRE(writer.persist(d) == ERROR_SUCCESS);
        one_write = watch.events();
    }
    CAPTURE(event_text(one_write));
    REQUIRE_FALSE(one_write.empty());

    const FileId id = file_id(writer.path());
    const uint64_t time = last_write(writer.path());
    const std::string on_disk = get(writer.path());
    {
        DirectoryWatch watch(s.dir);
        for (double db : {-3.0, -4.0, -5.0}) {
            d.state.preamp_db = db;
            REQUIRE(writer.apply(d) == ERROR_SUCCESS);
        }
        CHECK(writer.has_pending());
        const auto events = watch.events();
        CAPTURE(event_text(events));
        CHECK(events.empty());
    }
    CHECK(file_id(writer.path()) == id);
    CHECK(last_write(writer.path()) == time);
    CHECK(get(writer.path()) == on_disk);

    DirectoryWatch watch(s.dir);
    d.state.preamp_db = -6.0;
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    d.state.preamp_db = -7.0;
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    const auto events = watch.events();
    CAPTURE(event_text(events));
    CHECK(events == one_write);
    CHECK_FALSE(writer.has_pending());
    CHECK(preamp_in(get(writer.path()), kStereo) == -7.0);
}

TEST_CASE("flush writes a pending live edit, and nothing when none is pending") {
    Sandbox s;
    CompatWriter writer(s.dir);
    REQUIRE(writer.load() == ERROR_SUCCESS);
    DeviceConfig d = preamp_device(kStereo, -1.0);
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    d.state.preamp_db = -4.0;
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    REQUIRE(preamp_in(get(writer.path()), kStereo) == -1.0);

    REQUIRE(writer.flush() == ERROR_SUCCESS);
    CHECK_FALSE(writer.has_pending());
    CHECK(preamp_in(get(writer.path()), kStereo) == -4.0);

    DirectoryWatch watch(s.dir);
    REQUIRE(writer.flush() == ERROR_SUCCESS);
    CHECK(watch.events().empty());
}

TEST_CASE("a writer refuses a device whose channel count or rate was not set") {
    Sandbox s;
    CompatWriter writer(s.dir);
    REQUIRE(writer.load() == ERROR_SUCCESS);

    // Balance's speaker mute on the right channel.
    DeviceConfig unset;
    unset.endpoint_guid = kStereo;
    unset.state.speakers.muted = 1u << 1;
    CHECK(writer.persist(unset) == ERROR_INVALID_PARAMETER);
    CHECK(writer.apply(unset) == ERROR_INVALID_PARAMETER);
    DeviceConfig no_rate = unset;
    no_rate.layout = {2, 0x3};
    CHECK(writer.persist(no_rate) == ERROR_INVALID_PARAMETER);
    DeviceConfig no_layout = unset;
    no_layout.sample_rate = 48000.0;
    CHECK(writer.persist(no_layout) == ERROR_INVALID_PARAMETER);
    CHECK_FALSE(fs::exists(writer.path()));

    // What a guess would do: 7.1 writes the routing for 8 channels, which a
    // stereo device never plays.
    DeviceConfig guessed = no_layout;
    guessed.layout = ChannelLayout{8, 0x63F};
    guessed.state.speakers.swap_left_right = true;
    std::vector<std::vector<double>> input(2, std::vector<double>(64, 0.5));
    input[1].assign(64, 0.25);
    CHECK(UpstreamModel({2, 0x3}, 48000.0).run(format_device_block(guessed), input)[0][10] == 0.5);

    // A channel count no stream can have.
    DeviceConfig too_many = no_layout;
    too_many.layout = {kMaxApoChannels + 1, 0x3};
    CHECK(writer.apply(too_many) == ERROR_INVALID_PARAMETER);
    CHECK(writer.persist(too_many) == ERROR_INVALID_PARAMETER);
    CHECK_FALSE(fs::exists(writer.path()));

    DeviceConfig set = no_layout;
    set.layout = {2, 0x3};
    CHECK(writer.persist(set) == ERROR_SUCCESS);
    CHECK(UpstreamModel({2, 0x3}, 48000.0).run(get(writer.path()), input)[1][10] == 0.0);
}

TEST_CASE("isotone-compat takes paths outside the ANSI code page and writes UTF-8 JSON") {
    Sandbox s;
    // Escapes, not literal characters: the compiler reads this file as the ANSI
    // code page, which would turn literal UTF-8 into characters it does have.
    const fs::path root = s.dir / L"\u03A9\u65E5\u672C-caf\u00E9";
    fs::create_directories(root);
    put(root / "config.txt", "");
    const fs::path input = root / L"in-\u03A9.txt";
    put(input, "Preamp: -4 dB\n");

    const CliResult applied = run_cli({L"apply", L"--root", root.wstring(), L"--device", L"{798436D2-8C71-4834-9248-00CCBAACA00A}",
                                       L"--channels", L"2", L"--rate", L"48000", input.wstring()});
    CAPTURE(applied.out);
    CHECK(applied.exit_code == 0);
    REQUIRE(fs::exists(root / "Isotone.txt"));
    CHECK(preamp_in(get(root / "Isotone.txt"), kStereo) == -4.0);
    std::string escaped;
    for (char c : utf8_of(root / "Isotone.txt")) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    CHECK(applied.out.find(escaped) != std::string::npos);
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, applied.out.data(), static_cast<int>(applied.out.size()),
                              nullptr, 0) > 0);

    const CliResult inspected = run_cli({L"inspect", L"--root", root.wstring()});
    CAPTURE(inspected.out);
    CHECK(inspected.exit_code == 0);
    CHECK(inspected.out.find("\"ok\":true") != std::string::npos);
}

TEST_CASE("isotone-compat apply reads its input for the layout the text was written for") {
    Sandbox s;
    put(s.dir / "config.txt", "");
    const fs::path input = s.dir / "surround51.txt";
    // On 5.1 surround (L R C LFE SL SR) channel 6 is SR; on 7.1 it is RR.
    put(input, "Channel: SL\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n"
               "Channel: 6\nFilter 2: ON PK Fc 2000 Hz Gain -3 dB Q 1\n");
    const auto apply = [&](std::vector<std::wstring> text_layout) {
        std::vector<std::wstring> args = {L"apply", L"--root", s.dir.wstring(),
                                          L"--device", L"{798436D2-8C71-4834-9248-00CCBAACA00A}",
                                          L"--channels", L"8", L"--mask", L"0x63F", L"--rate", L"48000",
                                          L"--speakers", L"lip_sync_ms=1"};
        args.insert(args.end(), text_layout.begin(), text_layout.end());
        args.push_back(input.wstring());
        return run_cli(args);
    };

    const CliResult r = apply({L"--text-channels", L"6", L"--text-mask", L"0x60F"});
    CAPTURE(r.out);
    REQUIRE(r.exit_code == 0);
    const std::string text = get(s.dir / "Isotone.txt");
    CAPTURE(text);
    CHECK(text.find("# Isotone: layout 8 0x63f\n") != std::string::npos);
    CHECK(text.find("Channel: SL\nFilter 1: ON PK Fc 1000 Hz") != std::string::npos);
    CHECK(text.find("Channel: SR\nFilter 2: ON PK Fc 2000 Hz") != std::string::npos);

    // Without it the text is read for the device's layout.
    const CliResult device = apply({});
    CAPTURE(device.out);
    REQUIRE(device.exit_code == 0);
    CHECK(get(s.dir / "Isotone.txt").find("Channel: RR\nFilter 2: ON PK Fc 2000 Hz") != std::string::npos);

    // A mask with no channel count is refused.
    CHECK(apply({L"--text-mask", L"0x60F"}).exit_code == 2);
}

TEST_CASE("isotone-compat show reads for stereo unless a layout is given") {
    Sandbox s;
    put(s.dir / "config.txt", "");
    // A 5.1 block with FR and SL muted.
    put(s.dir / "flat.txt", "");
    const CliResult applied = run_cli({L"apply", L"--root", s.dir.wstring(), L"--device",
                                       L"{798436D2-8C71-4834-9248-00CCBAACA00A}", L"--channels", L"6", L"--mask",
                                       L"0x60F", L"--rate", L"48000", L"--speakers", L"muted=0x12",
                                       (s.dir / "flat.txt").wstring()});
    CAPTURE(applied.out);
    REQUIRE(applied.exit_code == 0);
    // Stereo has FR and no SL; 7.1 would put SL on channel 6 as well.
    const CliResult stereo = run_cli({L"show", L"--root", s.dir.wstring()});
    CAPTURE(stereo.out);
    REQUIRE(stereo.exit_code == 0);
    CHECK(stereo.out.find("muted=0x2") != std::string::npos);
    CHECK(stereo.out.find("muted=0x42") == std::string::npos);
    const CliResult surround = run_cli({L"show", L"--root", s.dir.wstring(), L"--channels", L"8", L"--mask", L"0x63F"});
    CAPTURE(surround.out);
    CHECK(surround.out.find("muted=0x42") != std::string::npos);
}

TEST_CASE("the discontinuity flag on the first packet after Start is not a glitch") {
    CHECK_FALSE(counts_as_glitch(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, true));
    CHECK(counts_as_glitch(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, false));
    CHECK(counts_as_glitch(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_SILENT, false));
    CHECK_FALSE(counts_as_glitch(AUDCLNT_BUFFERFLAGS_SILENT, false));
    CHECK_FALSE(counts_as_glitch(0, false));
}

TEST_CASE("loopback start gives up after its timeout, and reading while it stops is safe") {
    // No endpoint has this ID, so no audio device is opened.
    const std::string missing = "{00000000-0000-0000-0000-00000000f00d}";
    {
        LoopbackCapture capture;
        const auto t0 = std::chrono::steady_clock::now();
        const HRESULT hr = capture.start(missing, 0);
        CHECK(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
        CHECK_FALSE(capture.running());
    }
    // The abandoned thread finds no device and ends after its owner is gone.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    LoopbackCapture capture;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::thread reader([&] {
        std::vector<float> buf(size_t{64} * kMaxChannels);
        AudioRingCursor cursor;
        uint32_t channels = 0;
        while (!stop) {
            capture.read(&cursor, buf.data(), 64, &channels);
            capture.running();
            capture.discontinuities();
            reads++;
        }
    });
    int failed = 0;
    for (int i = 0; i < 200; ++i) failed += FAILED(capture.start(missing)) ? 1 : 0;
    stop = true;
    reader.join();
    CHECK(failed == 200);
    CHECK(reads > 0);
}
