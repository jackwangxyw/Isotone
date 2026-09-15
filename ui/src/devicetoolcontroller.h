// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Device operations for Devices, Settings Outputs and first run: one runner (one
// DevicetoolSession) on one worker thread for the app's life. Windows asks for
// approval once, at the first change. After a change: restart-audio, then test
// on the outputs changed, as Equalizer APO's Device Selector does; a Windows
// restart is offered only when restart-audio fails.
//
// phase, as the prototype's op box shows it:
//   ""          nothing
//   uac         waiting for approval
//   running     the command (Installing, Repairing, ...; or Applying)
//   restarting  restart-audio
//   testing     test on the outputs changed
//   done        finished (and audio restarted, if it was a change)
//   reboot      the change is done, restart-audio failed: Later, Restart Windows
//   failed      `reason`, a sentence from devicetool's JSON; Copy details, Retry
//   busy        exit 4, another devicetool run holds the machine lock; Retry
//   declined    approval declined; Retry
// kind: install, repair, uninstall, replace, test, approval, apply.
// Retry runs what failed: the change, or after it only the restart and tests
// (restart-audio busy), or only the test that failed.
//
// Restart Windows only emits restartWindowsRequested; the app connects it
// (main.cpp), tests cannot reach a reboot.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "devicetoolrunner.h"

// A reason as the UI shows it: a sentence with a capital and a full stop.
QString reasonSentence(QString s);
// Windows' own sentence for an error code.
QString windowsMessage(DWORD error);

// Settings Outputs' steps on Equalizer APO's config directory `dir`: attach
// (Peace kept), and removing the output's block from Isotone.txt. Empty, or the
// reason; an empty `dir` (no ConfigPath) is refused with "The system cannot find
// the path specified.", never read as the working directory.
QString attachConfigStep(const std::filesystem::path& dir);
QString removeBlockStep(const std::filesystem::path& dir, const QString& guid);

class DevicetoolController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Devicetool)
    QML_SINGLETON

    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString target READ target NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    // Approval given: the session runs, and changes ask nothing more.
    Q_PROPERTY(bool elevated READ elevated NOTIFY changed)
    // uac, running, restarting or testing.
    Q_PROPERTY(bool working READ working NOTIFY changed)
    // apply: per output GUID, queued, installing, removing, attaching,
    // installed, attached, removed or failed.
    Q_PROPERTY(QVariantMap rowStatus READ rowStatus NOTIFY changed)
    // apply: whether audio was restarted.
    Q_PROPERTY(bool restarted READ restarted NOTIFY changed)

public:
    // The runner from the environment: ISOTONE_FAKE_DEVICETOOL names a script
    // (ScriptedRunner), else the elevated session on devicetoolPath().
    explicit DevicetoolController(QObject* parent = nullptr);
    DevicetoolController(std::unique_ptr<DevicetoolRunner> runner, QObject* parent = nullptr);
    ~DevicetoolController() override;

    QString phase() const { return phase_; }
    QString kind() const { return kind_; }
    QString target() const { return target_; }
    QString reason() const { return reason_; }
    bool elevated() const { return elevated_; }
    bool working() const;
    QVariantMap rowStatus() const { return row_status_; }
    bool restarted() const { return restarted_; }

    // An operation from Devices.operation(): `kind` and devicetool's `args`, for
    // the output `guid`. test runs unelevated and restarts nothing.
    Q_INVOKABLE void run(const QString& kind, const QString& guid, const QStringList& args);
    // Asks for approval now (Settings Outputs' Change).
    Q_INVOKABLE void requestApproval();
    // Settings Outputs and first run: plans from Devices.plan(), in order.
    Q_INVOKABLE void apply(const QVariantList& plans);
    Q_INVOKABLE void retry();
    // Back to no phase, when nothing is working.
    Q_INVOKABLE void clear();
    Q_INVOKABLE void copyDetails();
    Q_INVOKABLE void restartWindows() { emit restartWindowsRequested(); }
    // Replaces the runner with a ScriptedRunner on `script` (tests). False when
    // working or the script does not parse.
    Q_INVOKABLE bool loadScript(const QString& script);

    // The last command's text, exit code and JSON (Copy details).
    QString details() const { return details_; }

signals:
    void changed();
    // A devicetool change or test ended: engines may have changed.
    void finished(const QString& kind, const QString& guid);
    void restartWindowsRequested();
    // apply changed an output's Off setting (equalizerapoconfig.h), before any
    // plan runs, and again if a plan that changed it does not succeed: Outputs
    // must be read again at once (Main.qml), so the session stops writing an
    // output turned Off before its block is removed.
    void outputChoicesChanged();

private:
    struct Request {
        QString kind;
        QString guid;
        QStringList args;
        QVariantList plans;
        // The change is done: only restart-audio, then test these.
        bool restart_only = false;
        QStringList tests;
    };
    enum class Outcome { ok, busy, failed, restart_failed };

    void start(const Request& request);
    void post(std::function<void()> job);
    void loop();
    // On the worker. publish() queues a change of state to the GUI thread.
    void publish(std::function<void(DevicetoolController*)> change);
    bool stopping();  // the app is closing: change nothing more
    bool approve();   // false when declined or failed, with the phase set
    // Runs one command and keeps its details; `reason` on busy or failed.
    Outcome command(const std::vector<std::wstring>& args, bool direct, DevicetoolResult* result, QString* reason);
    void fail(Outcome outcome, const QString& reason);
    Outcome restartAndTest(const QStringList& tests, QString* reason, QString* failed_test);
    void operate(const Request& request);
    // `restore`: the Off settings start() changed, as they were, by GUID.
    void applyPlans(const Request& request, bool changes, const QVariantMap& restore);
    void restoreChoices(const QVariantMap& restore);

    std::unique_ptr<DevicetoolRunner> runner_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;

    // GUI thread.
    QString phase_, kind_, target_, reason_, details_;
    bool elevated_ = false;
    bool restarted_ = false;
    QVariantMap row_status_;
    Request last_;
};
