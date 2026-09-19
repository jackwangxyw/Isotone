// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-devicetool through DevicetoolSession (serve), unelevated, and run
// directly. These run in CI on elevated runners, so every command that changes
// the registry or a service runs with --dry-run: run() and run_direct() refuse
// to start one without it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "doctest.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "session.h"

using isotone::devicetool::DevicetoolSession;

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// A minimal JSON reader, enough for devicetool's output (as in
// windows/devices/tests/test_devices.cpp).

struct Json {
    enum Type { null, boolean, number, string, array, object } type = null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const std::string& key) const {
        static const Json missing;
        const auto it = fields.find(key);
        return it == fields.end() ? missing : it->second;
    }
};

struct JsonParser {
    const std::string& t;
    size_t i = 0;
    bool ok = true;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < t.size() && t[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::string str() {
        std::string out;
        if (!eat('"')) {
            ok = false;
            return out;
        }
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\' && i < t.size()) {
                const char e = t[i++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'u': {
                        // devicetool escapes only control characters this way.
                        c = static_cast<char>(std::stoul(t.substr(i, 4), nullptr, 16));
                        i += 4;
                        break;
                    }
                    default: c = e;
                }
            }
            out += c;
        }
        ok = ok && i < t.size();
        ++i;
        return out;
    }
    Json value() {
        Json v;
        ws();
        if (i >= t.size()) {
            ok = false;
            return v;
        }
        if (t[i] == '{') {
            ++i;
            v.type = Json::object;
            if (eat('}')) return v;
            do {
                std::string key = str();
                if (!eat(':')) ok = false;
                v.fields[key] = value();
            } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (t[i] == '[') {
            ++i;
            v.type = Json::array;
            if (eat(']')) return v;
            do v.items.push_back(value());
            while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (t[i] == '"') {
            v.type = Json::string;
            v.s = str();
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = Json::boolean;
            v.b = true;
            i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = Json::boolean;
            i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            size_t used = 0;
            try {
                v.n = std::stod(t.substr(i), &used);
            } catch (...) {
                ok = false;
            }
            v.type = Json::number;
            i += used;
        }
        return v;
    }
};

bool parse_json(const std::string& text, Json* out) {
    JsonParser p{text};
    *out = p.value();
    p.ws();
    return p.ok && p.i == text.size();
}

bool has(const Json& array, const std::string& item) {
    return std::any_of(array.items.begin(), array.items.end(), [&](const Json& j) { return j.s == item; });
}

// ---------------------------------------------------------------------------

std::wstring tool() {
    const std::string compiled = ISOTONE_DEVICETOOL_EXE;
    return widen(compiled);
}

// The rule these tests keep on an elevated runner.
void require_dry_run_where_it_matters(const std::vector<std::wstring>& args) {
    static const wchar_t* const changing[] = {L"install", L"uninstall", L"repair", L"enable-enhancements", L"restart-audio",
                                                L"set-layout", L"machine-install", L"machine-uninstall"};
    const bool changes = std::any_of(args.begin(), args.end(), [](const std::wstring& a) {
        return std::any_of(std::begin(changing), std::end(changing), [&](const wchar_t* c) { return a == c; });
    });
    REQUIRE_MESSAGE((!changes || std::find(args.begin(), args.end(), L"--dry-run") != args.end()),
                    "a registry- or service-changing command without --dry-run");
}

DevicetoolSession::Result run(DevicetoolSession& session, const std::vector<std::wstring>& args) {
    require_dry_run_where_it_matters(args);
    return session.run(args);
}

std::wstring quote_argument(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

struct Direct {
    DWORD exit = STILL_ACTIVE;
    std::string out;
};

// Runs devicetool with `args` and its stdout captured; kills it (an unelevated
// child of this test) if it has not exited within `wait_ms`.
Direct run_direct(const std::vector<std::wstring>& args, DWORD wait_ms = 120000) {
    require_dry_run_where_it_matters(args);
    Direct d;
    std::wstring line = quote_argument(tool());
    for (const std::wstring& a : args) line += L" " + quote_argument(a);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    REQUIRE(CreatePipe(&read, &write, &inherit, 0));
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(tool().c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process);
    CloseHandle(write);
    if (!started) CloseHandle(read);
    REQUIRE_MESSAGE(started, "CreateProcess " << narrow(tool()) << " failed: " << GetLastError());
    std::thread reader([&] {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n > 0) d.out.append(buffer, n);
    });
    const DWORD waited = WaitForSingleObject(process.hProcess, wait_ms);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
    WaitForSingleObject(process.hProcess, INFINITE);
    reader.join();
    CloseHandle(read);
    GetExitCodeProcess(process.hProcess, &d.exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CHECK_MESSAGE(waited == WAIT_OBJECT_0, "devicetool did not exit within " << wait_ms << " ms");
    return d;
}

Json parsed(const std::string& text) {
    Json j;
    CHECK_MESSAGE(parse_json(trimmed(text), &j), "not JSON: " << text);
    CHECK(j.type == Json::object);
    return j;
}

bool render_key_exists() {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render",
                                         0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// The render endpoints devicetool lists, active ones first.
std::vector<std::string> render_endpoints() {
    const Direct list = run_direct({L"list"});
    Json j = parsed(list.out);
    std::vector<std::string> active, rest;
    for (const Json& e : j["endpoints"].items) {
        if (e["flow"].s != "render") continue;
        (has(e["state"]["flags"], "active") ? active : rest).push_back(e["guid"].s);
    }
    active.insert(active.end(), rest.begin(), rest.end());
    return active;
}

struct StartedSession {
    DevicetoolSession session;
    StartedSession() {
        const DWORD error = session.start(tool(), false, 10000);
        REQUIRE_MESSAGE(error == ERROR_SUCCESS, "start failed: " << error);
    }
};

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("a session runs list and returns what a direct run prints") {
    StartedSession s;
    CHECK(s.session.running());
    CHECK(s.session.process_id() != 0);
    const DevicetoolSession::Result r = run(s.session, {L"list"});
    REQUIRE(r.error == ERROR_SUCCESS);
    const Json j = parsed(r.json);
    CHECK(j["command"].s == "list");
    const Direct direct = run_direct({L"list"});
    CHECK(r.exit_code == static_cast<int>(direct.exit));
    CHECK(r.json == trimmed(direct.out));
    if (!render_key_exists()) {
        MESSAGE("SKIPPED exit 0: this machine has no MMDevices\\Audio\\Render key");
        return;
    }
    CHECK(r.exit_code == 0);
    CHECK(j["ok"].b);
}

TEST_CASE("status, install --dry-run and enable-enhancements --dry-run through a session match direct runs") {
    const std::vector<std::string> endpoints = render_endpoints();
    if (endpoints.empty()) {
        MESSAGE("SKIPPED: no render endpoints");
        return;
    }
    const std::wstring guid = widen(endpoints[0]);
    MESSAGE("endpoint " << endpoints[0]);
    StartedSession s;
    for (const std::vector<std::wstring>& args : std::vector<std::vector<std::wstring>>{
             {L"status", guid}, {L"install", guid, L"--dry-run"}, {L"enable-enhancements", guid, L"--dry-run"}}) {
        INFO(narrow(args[0]));
        const DevicetoolSession::Result r = run(s.session, args);
        REQUIRE(r.error == ERROR_SUCCESS);
        const Direct direct = run_direct(args);
        CHECK(r.exit_code == static_cast<int>(direct.exit));
        CHECK(r.json == trimmed(direct.out));
        const Json j = parsed(r.json);
        CHECK(j["command"].s == narrow(args[0]));
        if (args[0] == L"status") CHECK(r.exit_code == 0);
        if (args[0] != L"status" && j["ok"].b) CHECK(j["dry_run"].b);
        MESSAGE(narrow(args[0]) << ": exit " << r.exit_code << ", ok " << j["ok"].b << " " << j["reason"].s);
    }
}

TEST_CASE("restart-audio --dry-run through a session reads the service and restarts nothing") {
    StartedSession s;
    const DevicetoolSession::Result r = run(s.session, {L"restart-audio", L"--dry-run"});
    REQUIRE(r.error == ERROR_SUCCESS);
    CHECK(r.exit_code == 0);
    const Json j = parsed(r.json);
    CHECK(j["command"].s == "restart-audio");
    CHECK(j["ok"].b);
    CHECK(j["dry_run"].b);
    CHECK(j["service"].s == "AudioSrv");
    CHECK(j["state_before"].type == Json::string);
    CHECK((j["restarts"].items.size() >= 1 && j["restarts"].items.back().s == "AudioSrv"));
    CHECK(j["state_after"].type == Json::null);
    CHECK(j["seconds"].type == Json::null);
    MESSAGE("AudioSrv " << j["state_before"].s);
}

TEST_CASE("serve refuses what it does not run, and keeps serving") {
    StartedSession s;
    wchar_t temp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    const std::wstring output = std::wstring(temp) + L"isotone-devicetool-test-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    const std::vector<std::vector<std::wstring>> refused = {
        {L"frobnicate"},
        {L""},
        {L"serve", L"--pipe", L"x", L"--parent", L"4"},
        {L"roundtrip", L"{798436d2-8c71-4834-9248-00ccbaaca00a}"},
        {L"list", L"--output", output},
        {L"--output", output, L"list"},
        {L"status", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"--output"},
    };
    for (const std::vector<std::wstring>& args : refused) {
        INFO(narrow(args[0]));
        const DevicetoolSession::Result r = run(s.session, args);
        REQUIRE(r.error == ERROR_SUCCESS);
        CHECK(r.exit_code == 2);
        const Json j = parsed(r.json);
        CHECK_FALSE(j["ok"].b);
        CHECK(j["error"].s == "bad_arguments");
        MESSAGE(j["reason"].s);
    }
    const bool written = GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (written) DeleteFileW(output.c_str());
    CHECK_FALSE(written);

    // What the protocol cannot carry is refused before it is sent.
    CHECK(s.session.run({L"status", L"a\x1f" L"b"}).error == ERROR_INVALID_PARAMETER);
    CHECK(s.session.run({L"status", L"a\nb"}).error == ERROR_INVALID_PARAMETER);
    CHECK(s.session.run({}).error == ERROR_INVALID_PARAMETER);

    const DevicetoolSession::Result list = run(s.session, {L"list"});
    CHECK(list.error == ERROR_SUCCESS);
    CHECK(parsed(list.json)["command"].s == "list");
}

TEST_CASE("a request's arguments reach the command exactly") {
    StartedSession s;
    for (const std::wstring& arg : {std::wstring(L"a b\"c\\"), std::wstring(L"\\\\\"\\"), std::wstring(L"caf\u00e9 \u6f22"),
                                    std::wstring(L"")}) {
        const DevicetoolSession::Result r = run(s.session, {L"status", arg});
        REQUIRE(r.error == ERROR_SUCCESS);
        CHECK(r.exit_code == 2);
        CHECK(parsed(r.json)["reason"].s == "malformed endpoint " + narrow(arg));
    }
}

TEST_CASE("serve exits refusing a pipe whose server is not --parent") {
    const std::wstring name = L"Isotone.devicetool.test." + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    REQUIRE(pipe != INVALID_HANDLE_VALUE);
    // 4 is the System process, never this one.
    const Direct d = run_direct({L"serve", L"--pipe", name, L"--parent", L"4"}, 30000);
    CloseHandle(pipe);
    CHECK(d.exit == 1);
    const Json j = parsed(d.out);
    CHECK(j["command"].s == "serve");
    CHECK_FALSE(j["ok"].b);
    CHECK(j["reason"].s == "the pipe's server is process " + std::to_string(GetCurrentProcessId()) + ", not --parent 4");
}

TEST_CASE("serve refuses bad arguments and a pipe that is not there") {
    for (const std::vector<std::wstring>& args : std::vector<std::vector<std::wstring>>{
             {L"serve"},
             {L"serve", L"--pipe", L"x"},
             {L"serve", L"--parent", L"4"},
             {L"serve", L"--pipe", L"x", L"--parent", L"abc"},
             {L"serve", L"--pipe", L"x", L"--parent", L"0"},
             {L"serve", L"--pipe", L"x", L"--parent", L"99999999999"},
             {L"serve", L"--pipe", L"..\\x", L"--parent", L"4"},
             {L"serve", L"--pipe", L"..", L"--parent", L"4"},
             {L"serve", L"--pipe", L"x", L"--parent", L"4", L"{798436d2-8c71-4834-9248-00ccbaaca00a}"},
             {L"serve", L"--pipe", L"x", L"--parent", L"4", L"--dry-run"},
         }) {
        INFO(narrow(args.back()));
        const Direct d = run_direct(args, 30000);
        CHECK(d.exit == 2);
        CHECK(parsed(d.out)["error"].s == "bad_arguments");
    }
    const Direct missing = run_direct({L"serve", L"--pipe", L"Isotone.devicetool.test.missing", L"--parent", L"4"}, 30000);
    CHECK(missing.exit == 1);
    CHECK_FALSE(parsed(missing.out)["ok"].b);
}

TEST_CASE("serve exits 0 when the session closes") {
    StartedSession s;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.session.process_id());
    REQUIRE(process != nullptr);
    CHECK(run(s.session, {L"list"}).error == ERROR_SUCCESS);
    CHECK(WaitForSingleObject(process, 0) == WAIT_TIMEOUT);   // serving
    s.session.stop();
    CHECK_FALSE(s.session.running());
    CHECK(s.session.run({L"list"}).error == ERROR_INVALID_HANDLE);
    CHECK(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(process, &code);
    CHECK(code == 0);
    CloseHandle(process);
}

TEST_CASE("a session whose serve exits before connecting reports it") {
    // Not devicetool: whoami refuses serve's arguments and exits.
    DevicetoolSession session;
    wchar_t system[MAX_PATH];
    REQUIRE(GetSystemDirectoryW(system, MAX_PATH) > 0);
    const DWORD error = session.start(std::wstring(system) + L"\\whoami.exe", false, 10000);
    CHECK(error == ERROR_PIPE_NOT_CONNECTED);
    CHECK_FALSE(session.running());
}

TEST_CASE("enable-enhancements and restart-audio refuse bad arguments") {
    struct Case {
        std::vector<std::wstring> args;
        DWORD exit;
    };
    const std::vector<Case> cases = {
        {{L"enable-enhancements", L"--dry-run"}, 2},
        {{L"enable-enhancements", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"--mode", L"mfx", L"--dry-run"}, 2},
        {{L"enable-enhancements", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"--replace-equalizerapo", L"--dry-run"}, 2},
        {{L"enable-enhancements", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"--dry-run"}, 2},
        {{L"enable-enhancements", L"not-a-guid", L"--dry-run"}, 2},
        {{L"enable-enhancements", L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000001}", L"--dry-run"}, 1},
        {{L"enable-enhancements", L"{00000000-0000-0000-0000-000000000001}", L"--dry-run"}, 1},
        {{L"restart-audio", L"{798436d2-8c71-4834-9248-00ccbaaca00a}", L"--dry-run"}, 2},
        {{L"restart-audio", L"--mode", L"mfx", L"--dry-run"}, 2},
        {{L"restart-audio", L"--dry-run", L"--dry-run"}, 2},
    };
    for (const Case& c : cases) {
        std::string joined;
        for (const std::wstring& a : c.args) joined += narrow(a) + " ";
        INFO(joined);
        const Direct d = run_direct(c.args);
        CHECK(d.exit == c.exit);
        const Json j = parsed(d.out);
        CHECK_FALSE(j["ok"].b);
        CHECK(j["command"].s == narrow(c.args[0]));
    }
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The UI repairs the output the user picked: a refusal or a --mode for another
// endpoint must not decide that output's result.
TEST_CASE("repair <endpoint> --dry-run plans that endpoint alone, as the machine-wide repair plans it") {
    const Direct all_run = run_direct({L"repair", L"--dry-run"});
    const Json all = parsed(all_run.out);
    REQUIRE(all["command"].s == "repair");
    CHECK(all["dry_run"].b);
    size_t checked = 0, planned = 0;
    for (const std::string& endpoint : render_endpoints()) {
        INFO(endpoint);
        const Json* expected = nullptr;
        for (const Json& d : all["repaired"].items)
            if (lower(d["guid"].s) == lower(endpoint)) expected = &d;
        const Direct one = run_direct({L"repair", widen(endpoint), L"--dry-run"});
        const Json j = parsed(one.out);
        CHECK(j["command"].s == "repair");
        CHECK(j["dry_run"].b);
        const bool ok = !expected || (*expected)["ok"].b;
        CHECK(one.exit == (ok ? 0u : 1u));
        CHECK(j["ok"].b == ok);
        REQUIRE(j["repaired"].items.size() == (expected ? 1u : 0u));
        if (expected) {
            const Json& d = j["repaired"].items[0];
            CHECK(lower(d["guid"].s) == lower(endpoint));
            CHECK(d["ok"].b == (*expected)["ok"].b);
            CHECK(d["mode"].s == (*expected)["mode"].s);
            CHECK(d["error"].s == (*expected)["error"].s);
            CHECK(d["undid_interrupted"].s == (*expected)["undid_interrupted"].s);
            ++planned;
        }
        ++checked;
    }
    // A machine with no render endpoints (a CI runner) checks nothing here.
    MESSAGE(checked << " render endpoints, " << planned << " with something to repair");
}

TEST_CASE("repair's endpoint argument is checked as every command's is") {
    const std::wstring cable = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";
    struct Case {
        std::vector<std::wstring> args;
        DWORD exit;
    };
    const std::vector<Case> cases = {
        {{L"repair", L"not-a-guid", L"--dry-run"}, 2},
        {{L"repair", cable, cable, L"--dry-run"}, 2},
        {{L"repair", L"{00000000-0000-0000-0000-000000000001}", L"--dry-run"}, 1},
        {{L"repair", L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000001}", L"--dry-run"}, 1},
    };
    for (const Case& c : cases) {
        std::string joined;
        for (const std::wstring& a : c.args) joined += narrow(a) + " ";
        INFO(joined);
        const Direct d = run_direct(c.args);
        CHECK(d.exit == c.exit);
        const Json j = parsed(d.out);
        CHECK_FALSE(j["ok"].b);
        CHECK(j["command"].s == "repair");
    }
    // --mode is taken with an endpoint too. The mode is only in the answer where
    // the endpoint exists, so it is checked on one this machine has (none on a CI runner).
    const std::vector<std::string> endpoints = render_endpoints();
    const std::wstring endpoint = endpoints.empty() ? cable : widen(endpoints.front());
    const Direct with_mode = run_direct({L"repair", endpoint, L"--mode", L"mfx", L"--dry-run"});
    CHECK(with_mode.exit != 2);
    if (!endpoints.empty()) CHECK(parsed(with_mode.out)["mode"].s == "SFX_MFX");
}

TEST_CASE("enable-enhancements --dry-run and the status remedy on every endpoint") {
    const Direct list = run_direct({L"list"});
    const Json endpoints = parsed(list.out)["endpoints"];
    size_t render = 0, capture = 0, offered = 0;
    for (const Json& e : endpoints.items) {
        INFO(e["guid"].s << " " << e["connection"].s);
        const std::wstring guid = widen(e["guid"].s);
        const Direct run = run_direct({L"enable-enhancements", guid, L"--dry-run"});
        const Json j = parsed(run.out);
        if (e["flow"].s == "capture") {
            ++capture;
            CHECK(run.exit == 1);
            CHECK(j["reason"].s == "capture endpoints are not supported: IsoAPO is an output EQ");
            continue;
        }
        ++render;
        REQUIRE(run.exit == 0);
        CHECK(j["dry_run"].b);

        // The flag as the registry holds it, read here independently.
        const std::wstring fx = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid + L"\\FxProperties";
        DWORD type = REG_NONE, value = 0, size = sizeof(value);
        const LSTATUS read = RegGetValueW(HKEY_LOCAL_MACHINE, fx.c_str(), L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5",
                                          RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, &type, &value, &size);
        const bool present = read == ERROR_SUCCESS || read == ERROR_MORE_DATA;
        const bool disabled = read == ERROR_SUCCESS && type == REG_DWORD && value != 0;
        CHECK(j["changed"].b == present);
        CHECK(j["enhancements_were_disabled"].b == disabled);
        if (j["undid_interrupted"].type == Json::null) {
            REQUIRE(j["operations"].items.size() == (present ? 1u : 0u));
            if (present) {
                const Json& op = j["operations"].items[0];
                CHECK(op["op"].s == "delete value");
                CHECK(op["value"].s == "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5");
            }
        } else {
            MESSAGE("an interrupted " << j["undid_interrupted"].s << " was undone first; operations not compared");
        }

        const Direct status = run_direct({L"status", guid});
        const Json st = parsed(status.out);
        const Json& iso = st["isoapo"];
        const bool offers = has(iso["remedies"], "enable-enhancements");
        CHECK(offers == (iso["in_slots"].b && disabled && iso["state"].s != "interrupted"));
        if (st["device"].type == Json::object) CHECK(st["device"]["enhancements_disabled"].b == disabled);
        offered += offers;
    }
    MESSAGE(render << " render endpoints, " << capture << " capture refused, " << offered << " offered enable-enhancements");
    if (render == 0) MESSAGE("SKIPPED the render checks: no render endpoints");
}

TEST_CASE("roundtrip checks enable-enhancements with the flag set, clear and absent") {
    size_t checked = 0;
    for (const std::string& endpoint : render_endpoints()) {
        const std::wstring guid = widen(endpoint);
        const Json st = parsed(run_direct({L"status", guid}).out);
        const std::string state = st["isoapo"]["state"].s;
        // roundtrip stops before its checks on these.
        if (st["device"].type != Json::object || state == "unrecorded" || state == "interrupted") continue;
        INFO(endpoint << " " << st["device"]["connection"].s << ", " << state);
        const Direct d = run_direct({L"roundtrip", guid});
        const Json j = parsed(d.out);
        const Json& enhancements = j["enable_enhancements"];
        REQUIRE(enhancements.type == Json::object);
        CHECK(enhancements["ok"].b);
        for (const Json& f : enhancements["failed"].items) MESSAGE("failed: " << f.s);
        MESSAGE("roundtrip ok " << j["ok"].b);
        ++checked;
    }
    if (checked == 0) MESSAGE("SKIPPED: no present render endpoint roundtrip runs on");
}

TEST_CASE("layouts reads each output's speaker layouts, and set-layout --dry-run agrees with them") {
    struct Named {
        const char* name;
        double channels;
        const char* mask;
    };
    const Named layouts[] = {{"stereo", 2, "0x3"}, {"2.1", 3, "0xb"}, {"5.1", 6, "0x60f"}, {"7.1", 8, "0x63f"}};
    size_t read = 0, named = 0;
    StartedSession s;
    for (const std::string& endpoint : render_endpoints()) {
        const std::wstring guid = widen(endpoint);
        INFO(endpoint);
        const Direct d = run_direct({L"layouts", guid});
        const Json j = parsed(d.out);
        CHECK(j["command"].s == "layouts");
        // The session runs it the same way.
        const DevicetoolSession::Result r = run(s.session, {L"layouts", guid});
        REQUIRE(r.error == ERROR_SUCCESS);
        CHECK(r.exit_code == static_cast<int>(d.exit));
        if (d.exit != 0) {
            // An inactive endpoint, or one whose exclusive check says nothing.
            CHECK(d.exit == 1);
            CHECK(j["hresult"].type == Json::string);
            continue;
        }
        ++read;
        CHECK(j["ok"].b);
        REQUIRE(j["format"]["present"].b);

        // current names the layout whose channels and mask the format has, or none.
        const Named* own = nullptr;
        for (const Named& l : layouts)
            if (j["format"]["channels"].n == l.channels && j["format"]["channel_mask"].s == l.mask) own = &l;
        CHECK(j["current"].type == (own ? Json::string : Json::null));
        if (own) CHECK(j["current"].s == own->name);
        named += own != nullptr;

        // Every layout's dry run makes the same exclusive check: it passes exactly
        // for the supported ones, and sets nothing either way.
        for (const Json& n : j["supported"].items)
            CHECK(std::any_of(std::begin(layouts), std::end(layouts), [&](const Named& l) { return n.s == l.name; }));
        for (const Named& l : layouts) {
            INFO(l.name);
            const Direct c = run_direct({L"set-layout", guid, L"--layout", widen(l.name), L"--dry-run"});
            const Json k = parsed(c.out);
            CHECK(k["command"].s == "set-layout");
            CHECK(k["dry_run"].b);
            CHECK_FALSE(k["set_called"].b);
            CHECK(k["after"].type == Json::null);
            if (has(j["supported"], l.name)) {
                CHECK(c.exit == 0);
                CHECK(k["ok"].b);
                CHECK(k["policy_before"]["channels"].n == k["before"]["channels"].n);
                CHECK(k["requested"]["endpoint"]["channels"].n == l.channels);
                CHECK(k["requested"]["endpoint"]["channel_mask"].s == l.mask);
                CHECK(k["requested"]["mix"]["sample_format"].s == "float");
            } else {
                CHECK(c.exit == 1);
                CHECK_FALSE(k["ok"].b);
                CHECK(k["hresult"].s == "0x88890008");   // AUDCLNT_E_UNSUPPORTED_FORMAT
            }
        }
    }
    MESSAGE(read << " endpoints read, " << named << " with a named current layout");
    if (read == 0) MESSAGE("SKIPPED the layout checks: no render endpoint answered");
}

TEST_CASE("layouts and set-layout refuse bad arguments") {
    const std::wstring cable = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";
    struct Case {
        std::vector<std::wstring> args;
        DWORD exit;
    };
    const std::vector<Case> cases = {
        {{L"layouts"}, 2},
        {{L"layouts", cable, L"--dry-run"}, 2},
        {{L"layouts", cable, L"--layout", L"7.1"}, 2},
        {{L"set-layout", cable, L"--dry-run"}, 2},
        {{L"set-layout", cable, L"--layout", L"7.2", L"--dry-run"}, 2},
        {{L"set-layout", cable, L"--layout", L"Stereo", L"--dry-run"}, 2},
        {{L"set-layout", L"--layout", L"7.1", L"--dry-run"}, 2},
        {{L"set-layout", L"not-a-guid", L"--layout", L"7.1", L"--dry-run"}, 2},
        {{L"set-layout", L"{00000000-0000-0000-0000-000000000001}", L"--layout", L"7.1", L"--dry-run"}, 1},
        {{L"set-layout", L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000001}", L"--layout", L"7.1", L"--dry-run"}, 1},
    };
    for (const Case& c : cases) {
        std::string joined;
        for (const std::wstring& a : c.args) joined += narrow(a) + " ";
        INFO(joined);
        const Direct d = run_direct(c.args);
        CHECK(d.exit == c.exit);
        const Json j = parsed(d.out);
        CHECK_FALSE(j["ok"].b);
        CHECK(j["command"].s == narrow(c.args[0]));
    }
}

// ---------------------------------------------------------------------------
// The machine-wide half of an install (stage 6). Every case here is a dry run:
// these commands write HKLM and the ProgramData ACL, and run_direct's
// require_dry_run_where_it_matters refuses them without --dry-run.

TEST_CASE("machine-install --dry-run refuses a DLL audiodg could not load") {
    struct Case {
        std::wstring dll;
        const char* failing;   // the check that must be false, or nullptr for a path refused outright
        DWORD exit;
    };
    const std::vector<Case> cases = {
        {L"IsoAPO.dll", nullptr, 1},                                  // relative
        {L"\\\\server\\share\\IsoAPO.dll", nullptr, 1},               // UNC: audiodg would not reach it
        {L"C:\\does\\not\\exist\\IsoAPO.dll", "the DLL exists", 1},
    };
    for (const Case& c : cases) {
        INFO(narrow(c.dll));
        const Direct d = run_direct({L"machine-install", L"--dll", c.dll, L"--dry-run"});
        CHECK(d.exit == c.exit);
        const Json j = parsed(d.out);
        CHECK_FALSE(j["ok"].b);
        CHECK(j["command"].s == "machine-install");
        CHECK(j["dry_run"].b);
        if (c.failing != nullptr) {
            bool found = false;
            for (const Json& check : j["checks"].items) {
                if (check["check"].s == c.failing) {
                    found = true;
                    CHECK_FALSE(check["ok"].b);
                }
            }
            CHECK_MESSAGE(found, "no check named " << c.failing);
        }
    }
}

TEST_CASE("machine-install --dry-run plans the ACL audiodg and every account need") {
    // A path that cannot be registered, so this runs the same everywhere,
    // including a CI runner with no IsoAPO on it: the plan is reported whether
    // or not the DLL passes, because a run that stops at a bad DLL should still
    // say what the whole install would do.
    const Direct d = run_direct({L"machine-install", L"--dll", L"C:\\Isotone\\IsoAPO.dll", L"--dry-run"});
    const Json j = parsed(d.out);
    CHECK(j["dry_run"].b);
    CHECK(j["data_dir"].s.find("IsoAPO") != std::string::npos);

    const std::string acl = j["acl"].s;
    INFO(acl);
    // Protected, so ProgramData's read-and-create-only inheritance does not
    // apply as well, and inherited by what is created inside it.
    CHECK(acl.rfind("D:P", 0) == 0);
    CHECK(acl.find("(A;OICI;FA;;;SY)") != std::string::npos);
    CHECK(acl.find("(A;OICI;FA;;;BA)") != std::string::npos);
    // LOCAL SERVICE, which audiodg runs as, reads and executes.
    CHECK(acl.find("(A;OICI;0x1200a9;;;LS)") != std::string::npos);
    // Authenticated Users modify, which is DELETE as well as write: the app
    // replaces the state file with MoveFileEx, and without DELETE on the file
    // already there one account cannot replace another's (docs/ui-spec.md).
    CHECK(acl.find("(A;OICI;0x1301bf;;;AU)") != std::string::npos);
    // The Windows Audio service by its own SID, resolved from the name rather
    // than written out: a wrong literal is an ACE that silently grants nothing.
    CHECK(acl.find(";;;S-1-5-80-") != std::string::npos);
}

TEST_CASE("machine-install --dry-run says what it would do and does none of it") {
    const Direct registered = run_direct({L"status", L"{798436d2-8c71-4834-9248-00ccbaaca00a}"});
    const Json status = parsed(registered.out);
    const std::string dll = status["isoapo"]["registration"]["dll"].s;
    if (dll.empty()) return;   // nothing registered here, so nothing that passes the DLL checks

    const Direct d = run_direct({L"machine-install", L"--dll", widen(dll), L"--dry-run"});
    const Json j = parsed(d.out);
    if (!j["ok"].b) return;    // a registered DLL that is gone or unreadable: the case above covers that
    CHECK(has(j["would"], "call DllRegisterServer in the DLL"));
    CHECK(has(j["would"], "create the data directory and set its ACL"));
    // The value is only set when it is not already 1, and this machine's state
    // decides which; either way the step matches what was read.
    CHECK(has(j["would"], "set DisableProtectedAudioDG=1") != j["protected_audiodg_already_disabled"].b);
}

TEST_CASE("machine-uninstall --dry-run leaves DisableProtectedAudioDG that is not ours") {
    const Direct d = run_direct({L"machine-uninstall", L"--dry-run"});
    CHECK(d.exit == 0);
    const Json j = parsed(d.out);
    CHECK(j["ok"].b);
    CHECK(j["dry_run"].b);
    CHECK_FALSE(j["remove_data"].b);   // the saved state stays unless it is asked for

    // Equalizer APO sets the same value and stops working without it, so the
    // value is only ever removed when this install is what set it and nothing
    // else needs it.
    const bool ours = j["protected_audiodg_set_by_isotone"].b;
    const bool eapo = j["equalizerapo_installed"].b;
    CHECK(j["restore_protected_audio"].b == (ours && !eapo));

    bool says_why = false;
    for (const Json& step : j["would"].items) {
        if (step.s.rfind("remove DisableProtectedAudioDG", 0) == 0 ||
            step.s.rfind("leave DisableProtectedAudioDG", 0) == 0) {
            says_why = true;
        }
    }
    CHECK(says_why);
}

TEST_CASE("machine-uninstall --dry-run says what --remove-data would take") {
    const Direct kept = run_direct({L"machine-uninstall", L"--dry-run"});
    const Direct removed = run_direct({L"machine-uninstall", L"--remove-data", L"--dry-run"});
    const Json a = parsed(kept.out), b = parsed(removed.out);
    CHECK_FALSE(a["remove_data"].b);
    CHECK(b["remove_data"].b);
    CHECK(has(a["would"], "leave the data directory and the saved state in it"));
    CHECK(has(b["would"], "delete the data directory and every saved state in it"));
}

TEST_CASE("machine-install and machine-uninstall refuse bad arguments") {
    const std::wstring cable = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";
    // Never reached: every case here is refused for its arguments first.
    const wchar_t* const kAnyDll = L"C:\\Isotone\\IsoAPO.dll";
    const std::vector<std::vector<std::wstring>> cases = {
        {L"machine-install", L"--dry-run"},                                    // no --dll
        {L"machine-install", L"--dll", kAnyDll, cable, L"--dry-run"},          // takes no endpoint
        {L"machine-install", L"--dll", kAnyDll, L"--remove-data", L"--dry-run"},
        {L"machine-uninstall", L"--dll", kAnyDll, L"--dry-run"},
        {L"machine-uninstall", cable, L"--dry-run"},
        {L"machine-uninstall", L"--mode", L"mfx", L"--dry-run"},
    };
    for (const std::vector<std::wstring>& args : cases) {
        std::string joined;
        for (const std::wstring& a : args) joined += narrow(a) + " ";
        INFO(joined);
        const Direct d = run_direct(args);
        CHECK(d.exit == 2);
    }
}
