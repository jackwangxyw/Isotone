// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicetoolrunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <thread>

namespace {

QString& path_override() {
    static QString path;
    return path;
}

// As CommandLineToArgvW reads it back (devicetool's quote_argument).
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

}  // namespace

QString devicetoolPath() {
    if (!path_override().isEmpty()) return path_override();
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("isotone-devicetool.exe"));
    if (QFileInfo::exists(beside)) return QDir::toNativeSeparators(beside);
#ifdef ISOTONE_DEVICETOOL_BUILD_PATH
    return QDir::toNativeSeparators(QStringLiteral(ISOTONE_DEVICETOOL_BUILD_PATH));
#else
    return QDir::toNativeSeparators(beside);
#endif
}

void setDevicetoolPath(const QString& path) { path_override() = path; }

DevicetoolResult runDevicetoolDirect(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD timeout_ms) {
    DevicetoolResult r;
    std::wstring line = quote_argument(exe);
    for (const std::wstring& a : args) line += L" " + quote_argument(a);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        r.error = GetLastError();
        return r;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    const DWORD start_error = GetLastError();
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        r.error = start_error;
        return r;
    }
    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n > 0) output.append(buffer, n);
    });
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
    WaitForSingleObject(process.hProcess, INFINITE);
    reader.join();
    CloseHandle(read);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0) {
        r.error = ERROR_TIMEOUT;
        return r;
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) output.pop_back();
    r.exit_code = static_cast<int>(code);
    r.json = std::move(output);
    return r;
}

// ---------------------------------------------------------------------------

DWORD SessionRunner::start() {
    // 60 s for serve to connect once the prompt is answered.
    return session_.start(exe_, elevate_, 60000);
}

DevicetoolResult SessionRunner::run(const std::vector<std::wstring>& args) {
    const isotone::devicetool::DevicetoolSession::Result s = session_.run(args);
    return {s.error, s.exit_code, s.json};
}

DevicetoolResult SessionRunner::runDirect(const std::vector<std::wstring>& args) { return runDevicetoolDirect(exe_, args); }

// ---------------------------------------------------------------------------

bool ScriptedRunner::load(const QByteArray& script) {
    const QJsonDocument doc = QJsonDocument::fromJson(script);
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();
    const auto answer_of = [](const QJsonObject& a) {
        Answer answer;
        answer.error = static_cast<DWORD>(a.value(QStringLiteral("error")).toInteger(0));
        answer.exit_code = a.value(QStringLiteral("exit")).toInt(0);
        answer.delay_ms = a.value(QStringLiteral("delay_ms")).toInt(0);
        if (a.value(QStringLiteral("json")).isObject())
            answer.json = QJsonDocument(a.value(QStringLiteral("json")).toObject()).toJson(QJsonDocument::Compact).toStdString();
        return answer;
    };
    std::lock_guard lock(mutex_);
    started_ = o.value(QStringLiteral("started")).toBool(false);
    start_ = answer_of(o.value(QStringLiteral("start")).toObject());
    answers_.clear();
    used_.clear();
    const QJsonObject commands = o.value(QStringLiteral("commands")).toObject();
    for (auto it = commands.begin(); it != commands.end(); ++it) {
        for (const QJsonValue& a : it.value().toArray()) answers_[it.key().toStdString()].push_back(answer_of(a.toObject()));
    }
    return true;
}

void ScriptedRunner::setStart(DWORD error, int delay_ms) {
    std::lock_guard lock(mutex_);
    start_ = {error, 0, {}, delay_ms};
}

void ScriptedRunner::answer(const std::string& command, const Answer& a) {
    std::lock_guard lock(mutex_);
    auto& list = answers_[command];
    // Answers not yet used stay ahead of this one; a used last answer is replaced.
    if (!list.empty() && used_[command] >= list.size()) {
        list.clear();
        used_[command] = 0;
    }
    list.push_back(a);
}

void ScriptedRunner::wait(int delay_ms) {
    if (delay_ms == 0) return;
    std::unique_lock lock(mutex_);
    if (delay_ms < 0) {
        wake_.wait(lock, [this] { return cancelled_; });
    } else {
        wake_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this] { return cancelled_; });
    }
}

DWORD ScriptedRunner::start() {
    Answer a;
    {
        std::lock_guard lock(mutex_);
        a = start_;
    }
    wait(a.delay_ms);
    if (a.error == ERROR_SUCCESS) started_ = true;
    return a.error;
}

DevicetoolResult ScriptedRunner::reply(const std::vector<std::wstring>& args, bool direct) {
    const std::string command = args.empty() ? std::string() : QString::fromStdWString(args[0]).toStdString();
    Answer a{ERROR_SUCCESS, 0, "{\"command\":\"" + command + "\",\"ok\":true}", 0};
    {
        std::lock_guard lock(mutex_);
        calls_.push_back({args, direct});
        const auto it = answers_.find(command);
        if (it != answers_.end() && !it->second.empty()) {
            size_t& used = used_[command];
            a = it->second[std::min(used, it->second.size() - 1)];
            if (used < it->second.size()) ++used;
        }
    }
    wait(a.delay_ms);
    return {a.error, a.exit_code, a.json};
}

DevicetoolResult ScriptedRunner::run(const std::vector<std::wstring>& args) { return reply(args, false); }
DevicetoolResult ScriptedRunner::runDirect(const std::vector<std::wstring>& args) { return reply(args, true); }

void ScriptedRunner::cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    wake_.notify_all();
}

std::vector<ScriptedRunner::Call> ScriptedRunner::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}
