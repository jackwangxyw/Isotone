// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The UI's side of `isotone-devicetool serve`: Windows asks for approval once,
// when start() launches the elevated devicetool, and every run() until stop()
// or the app's exit is a devicetool command in that process. The protocol is
// described at cmd_serve in main.cpp.
//
// Calls are made from one worker thread. run() blocks for as long as the
// command takes (an install waits up to a minute for the machine lock), and no
// call may overlap another.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <vector>

namespace isotone::devicetool {

class DevicetoolSession {
public:
    struct Result {
        DWORD error = ERROR_SUCCESS;   // the session's own failure; exit_code and json are only meaningful without one
        int exit_code = -1;            // the command's exit code: 0 ok, 1 failed or refused, 2 bad arguments, 3 not elevated, 4 busy
        std::string json;              // the command's JSON object, UTF-8; empty if it wrote none
    };

    DevicetoolSession() = default;
    ~DevicetoolSession() { stop(); }
    DevicetoolSession(const DevicetoolSession&) = delete;
    DevicetoolSession& operator=(const DevicetoolSession&) = delete;

    // Creates the pipe and launches `devicetool_exe serve`: elevated with the
    // "runas" verb, or as a plain child (tests). ShellExecuteEx returns once the
    // user has answered Windows' prompt; then start waits up to `timeout_ms` for
    // serve to connect. ERROR_CANCELLED when the user declines, ERROR_PIPE_NOT_CONNECTED
    // when serve exited without connecting, ERROR_TIMEOUT, ERROR_ACCESS_DENIED
    // when the process that connected is not the one launched, or the error of
    // the call that failed.
    DWORD start(const std::wstring& devicetool_exe, bool elevate, DWORD timeout_ms);

    // One command with its arguments, as on the command line, without --output.
    // An argument may not contain U+001F or a line feed (ERROR_INVALID_PARAMETER).
    // After a failure of the pipe itself the session is stopped, and every later
    // call fails with ERROR_INVALID_HANDLE.
    Result run(const std::vector<std::wstring>& args);

    // Closes the pipe. serve exits once the command it is running, if any, has
    // finished. Does not wait for it.
    void stop();

    bool running() const { return pipe_ != INVALID_HANDLE_VALUE; }
    DWORD process_id() const { return process_id_; }   // serve's, while running

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    DWORD process_id_ = 0;
};

}  // namespace isotone::devicetool
