// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "session.h"

#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

#include <cstdlib>

namespace isotone::devicetool {

namespace {

// Waits for one read or write on the overlapped pipe handle.
DWORD transfer(HANDLE pipe, bool write, char* data, DWORD size, DWORD* done) {
    *done = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return GetLastError();
    const BOOL finished = write ? WriteFile(pipe, data, size, nullptr, &overlapped)
                                : ReadFile(pipe, data, size, nullptr, &overlapped);
    DWORD error = finished ? ERROR_SUCCESS : GetLastError();
    if (finished || error == ERROR_IO_PENDING)
        error = GetOverlappedResult(pipe, &overlapped, done, TRUE) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(overlapped.hEvent);
    return error;
}

// D:P, the current user, SYSTEM and Administrators: another user's process
// cannot open the pipe, and the user's own processes find its one instance taken
// by serve. Administrators covers serve running as another account, when a
// standard user types an administrator's password at the prompt; an unelevated
// token holds that group deny-only, so it gains nothing from it.
DWORD pipe_security(PSECURITY_DESCRIPTOR* sd) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    const BOOL got = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    const DWORD token_error = GetLastError();
    CloseHandle(token);
    if (!got) return token_error;
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return GetLastError();
    const std::wstring sddl = std::wstring(L"D:P(A;;GRGW;;;") + sid + L")(A;;GRGW;;;SY)(A;;GRGW;;;BA)";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, sd, nullptr))
        return GetLastError();
    return ERROR_SUCCESS;
}

}  // namespace

DWORD DevicetoolSession::start(const std::wstring& devicetool_exe, bool elevate, DWORD timeout_ms) {
    if (running()) return ERROR_ALREADY_INITIALIZED;

    GUID guid{};
    const HRESULT hr = CoCreateGuid(&guid);
    if (FAILED(hr)) return static_cast<DWORD>(hr);
    wchar_t guid_text[40];
    StringFromGUID2(guid, guid_text, 40);
    const std::wstring name = std::wstring(L"Isotone.devicetool.") + guid_text;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (const DWORD error = pipe_security(&sd); error != ERROR_SUCCESS) return error;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    // One instance, which must be the first: a pipe of this name made by anyone
    // else fails the create rather than being joined.
    HANDLE pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                                   65536, 0, &sa);
    const DWORD create_error = GetLastError();
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) return create_error;

    std::wstring parameters = L"serve --pipe " + name + L" --parent " + std::to_wstring(GetCurrentProcessId());
    HANDLE process = nullptr;
    if (elevate) {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = devicetool_exe.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) {
            const DWORD error = GetLastError();   // ERROR_CANCELLED when the user declines
            CloseHandle(pipe);
            return error;
        }
        process = info.hProcess;
        if (process == nullptr) {
            CloseHandle(pipe);
            return ERROR_INVALID_HANDLE;
        }
    } else {
        std::wstring command_line = L"\"" + devicetool_exe + L"\" " + parameters;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION started{};
        if (!CreateProcessW(devicetool_exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &started)) {
            const DWORD error = GetLastError();
            CloseHandle(pipe);
            return error;
        }
        CloseHandle(started.hThread);
        process = started.hProcess;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = overlapped.hEvent == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (error == ERROR_SUCCESS && !ConnectNamedPipe(pipe, &overlapped)) {
        error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            error = ERROR_SUCCESS;
        } else if (error == ERROR_IO_PENDING) {
            const HANDLE waits[] = {overlapped.hEvent, process};
            const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
            DWORD ignored = 0;
            if (waited == WAIT_OBJECT_0) {
                error = GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) ? ERROR_SUCCESS : GetLastError();
            } else {
                error = waited == WAIT_OBJECT_0 + 1 ? ERROR_PIPE_NOT_CONNECTED
                        : waited == WAIT_TIMEOUT    ? ERROR_TIMEOUT
                                                    : GetLastError();
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            }
        }
    }
    if (overlapped.hEvent != nullptr) CloseHandle(overlapped.hEvent);

    // The pipe is open to the user's other processes too: only the process just
    // launched may be the client.
    ULONG client = 0;
    if (error == ERROR_SUCCESS && (!GetNamedPipeClientProcessId(pipe, &client) || client != GetProcessId(process)))
        error = ERROR_ACCESS_DENIED;
    if (error != ERROR_SUCCESS) {
        CloseHandle(pipe);
        CloseHandle(process);
        return error;
    }
    pipe_ = pipe;
    process_ = process;
    process_id_ = client;
    return ERROR_SUCCESS;
}

DevicetoolSession::Result DevicetoolSession::run(const std::vector<std::wstring>& args) {
    Result r;
    if (!running()) {
        r.error = ERROR_INVALID_HANDLE;
        return r;
    }
    if (args.empty()) {
        r.error = ERROR_INVALID_PARAMETER;
        return r;
    }
    std::string request;
    for (const std::wstring& arg : args) {
        if (arg.find_first_of(L"\x1f\n") != std::wstring::npos) {
            r.error = ERROR_INVALID_PARAMETER;
            return r;
        }
        std::string field;
        if (!arg.empty()) {
            const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg.data(), static_cast<int>(arg.size()),
                                              nullptr, 0, nullptr, nullptr);
            if (n == 0) {
                r.error = ERROR_NO_UNICODE_TRANSLATION;
                return r;
            }
            field.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg.data(), static_cast<int>(arg.size()), field.data(), n,
                                nullptr, nullptr);
        }
        request += (request.empty() ? "" : "\x1f") + field;
    }
    request += '\n';

    for (size_t written = 0; written < request.size();) {
        DWORD n = 0;
        r.error = transfer(pipe_, true, request.data() + written, static_cast<DWORD>(request.size() - written), &n);
        if (r.error != ERROR_SUCCESS) {
            stop();
            return r;
        }
        written += n;
    }
    std::string response;
    while (response.empty() || response.back() != '\n') {
        char buffer[4096];
        DWORD n = 0;
        r.error = transfer(pipe_, false, buffer, sizeof(buffer), &n);
        if (r.error == ERROR_SUCCESS && n == 0) r.error = ERROR_BROKEN_PIPE;
        if (r.error != ERROR_SUCCESS) {
            stop();
            return r;
        }
        response.append(buffer, n);
    }

    // {"exit":<code>,"result":<object or null>}\n, as serve writes it.
    const std::string exit_field = "{\"exit\":", result_field = ",\"result\":";
    const size_t result_at = response.find(result_field);
    char* end = nullptr;
    const long code = response.rfind(exit_field, 0) == 0 && result_at != std::string::npos
                          ? std::strtol(response.c_str() + exit_field.size(), &end, 10)
                          : 0;
    if (end == nullptr || end != response.c_str() + result_at || response.size() < result_at + result_field.size() + 2 ||
        response.compare(response.size() - 2, 2, "}\n") != 0 || response.find('\n') != response.size() - 1) {
        r.error = ERROR_INVALID_DATA;
        stop();
        return r;
    }
    r.exit_code = static_cast<int>(code);
    r.json = response.substr(result_at + result_field.size(), response.size() - 2 - result_at - result_field.size());
    if (r.json == "null") r.json.clear();
    return r;
}

void DevicetoolSession::stop() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    if (process_ != nullptr) CloseHandle(process_);
    pipe_ = INVALID_HANDLE_VALUE;
    process_ = nullptr;
    process_id_ = 0;
}

}  // namespace isotone::devicetool
