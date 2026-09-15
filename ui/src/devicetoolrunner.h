// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// How the UI reaches isotone-devicetool (docs/ui-spec.md, "Framework"): changing
// commands in one elevated `serve` for the life of the app (DevicetoolSession),
// read-only ones (status, test) as a plain child process. DevicetoolController
// drives a runner from its one worker thread; the scripted runner stands in for
// devicetool in tests and screenshots.

#pragma once

#include <windows.h>

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "session.h"

struct DevicetoolResult {
    DWORD error = ERROR_SUCCESS;   // the runner's own failure; exit_code and json are meaningless with one
    int exit_code = -1;            // 0 ok, 1 failed, 2 bad arguments, 3 not elevated, 4 busy
    std::string json;              // the command's JSON object, UTF-8
};

class DevicetoolRunner {
public:
    virtual ~DevicetoolRunner() = default;
    // Starts the elevated session: Windows asks for approval. ERROR_SUCCESS,
    // ERROR_CANCELLED when declined, or another error.
    virtual DWORD start() = 0;
    virtual bool started() const = 0;
    // A command in the session (start first).
    virtual DevicetoolResult run(const std::vector<std::wstring>& args) = 0;
    // A read-only command as a child process of the app, unelevated.
    virtual DevicetoolResult runDirect(const std::vector<std::wstring>& args) = 0;
    // Ends a wait the runner can end (the scripted runner's held commands); a
    // real command runs to its end.
    virtual void cancel() {}
};

// isotone-devicetool.exe next to the app, else the one this build made; or the
// path given to setDevicetoolPath.
QString devicetoolPath();
void setDevicetoolPath(const QString& path);

// Runs `exe args` unelevated with its output read, and waits up to `timeout_ms`
// (then kills it: ERROR_TIMEOUT).
DevicetoolResult runDevicetoolDirect(const std::wstring& exe, const std::vector<std::wstring>& args,
                                     DWORD timeout_ms = 120000);

class SessionRunner : public DevicetoolRunner {
public:
    // `elevate` false launches serve as a plain child (tests): every changing
    // command then answers exit code 3.
    SessionRunner(std::wstring exe, bool elevate) : exe_(std::move(exe)), elevate_(elevate) {}
    DWORD start() override;
    bool started() const override { return session_.running(); }
    DevicetoolResult run(const std::vector<std::wstring>& args) override;
    DevicetoolResult runDirect(const std::vector<std::wstring>& args) override;

private:
    std::wstring exe_;
    bool elevate_;
    isotone::devicetool::DevicetoolSession session_;
};

// Answers from a script instead of devicetool (tests, --fake-devicetool). Per
// command name, a list of answers used in order, the last one repeating; a
// command with none answers exit 0 and {"command":<name>,"ok":true}. The script
// as JSON:
//   {"started": false,                                   already approved
//    "start": {"error": 0, "delay_ms": 0},                the approval: 1223 is declined
//    "commands": {"install": [{"exit": 0, "json": {...}, "delay_ms": 0, "error": 0}], ...}}
// delay_ms -1 holds the answer until cancel().
class ScriptedRunner : public DevicetoolRunner {
public:
    struct Answer {
        DWORD error = ERROR_SUCCESS;
        int exit_code = 0;
        std::string json;
        int delay_ms = 0;
    };
    struct Call {
        std::vector<std::wstring> args;
        bool direct = false;
    };

    ScriptedRunner() = default;
    // False when `script` is not a JSON object.
    bool load(const QByteArray& script);

    void setStart(DWORD error, int delay_ms = 0);
    void setStarted(bool started) { started_ = started; }
    void answer(const std::string& command, const Answer& a);

    DWORD start() override;
    bool started() const override { return started_; }
    DevicetoolResult run(const std::vector<std::wstring>& args) override;
    DevicetoolResult runDirect(const std::vector<std::wstring>& args) override;
    void cancel() override;

    std::vector<Call> calls() const;

private:
    DevicetoolResult reply(const std::vector<std::wstring>& args, bool direct);
    void wait(int delay_ms);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool cancelled_ = false;
    std::atomic<bool> started_{false};
    Answer start_;
    std::map<std::string, std::vector<Answer>> answers_;
    std::map<std::string, size_t> used_;
    std::vector<Call> calls_;
};
