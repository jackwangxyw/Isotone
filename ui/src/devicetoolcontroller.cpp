// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicetoolcontroller.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include "compat_writer.h"
#include "config_attach.h"
#include "equalizerapoconfig.h"

namespace {

std::vector<std::wstring> wide(const QStringList& args) {
    std::vector<std::wstring> out;
    for (const QString& a : args) out.push_back(a.toStdWString());
    return out;
}

QString joined(const std::vector<std::wstring>& args) {
    QStringList text;
    for (const std::wstring& a : args) text << QString::fromStdWString(a);
    return text.join(QLatin1Char(' '));
}

const char* verb_of(const QStringList& command) {
    return command.value(0) == QLatin1String("uninstall") ? "removing" : "installing";
}

}  // namespace

QString reasonSentence(QString s) {
    s = s.trimmed();
    if (s.isEmpty()) return s;
    s[0] = s[0].toUpper();
    if (!s.endsWith(QLatin1Char('.')) && !s.endsWith(QLatin1Char('?')) && !s.endsWith(QLatin1Char('!'))) s += QLatin1Char('.');
    return s;
}

QString windowsMessage(DWORD error) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    const QString s = n > 0 ? QString::fromWCharArray(text, static_cast<qsizetype>(n)) : QStringLiteral("Windows error %1").arg(error);
    if (text) LocalFree(text);
    return reasonSentence(s);
}

namespace {

QString reason_of(const DevicetoolResult& r) {
    if (r.error != ERROR_SUCCESS) return windowsMessage(r.error);
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object();
    QString reason = o.value(QStringLiteral("reason")).toString();
    // repair: the reason is on the endpoint that failed.
    const QJsonArray repaired = o.value(QStringLiteral("repaired")).toArray();
    for (qsizetype i = 0; reason.isEmpty() && i < repaired.size(); ++i)
        reason = repaired[i].toObject().value(QStringLiteral("error")).toString();
    if (reason.isEmpty()) reason = QStringLiteral("Exit code %1").arg(r.exit_code);
    return reasonSentence(reason);
}

}  // namespace

DevicetoolController::DevicetoolController(QObject* parent) : QObject(parent) {
    const QString script = qEnvironmentVariable("ISOTONE_FAKE_DEVICETOOL");
    if (!script.isEmpty()) {
        auto scripted = std::make_unique<ScriptedRunner>();
        QFile f(script);
        if (!f.open(QIODevice::ReadOnly) || !scripted->load(f.readAll()))
            qWarning("ISOTONE_FAKE_DEVICETOOL: %s is not a script", qPrintable(script));
        runner_ = std::move(scripted);
    } else {
        runner_ = std::make_unique<SessionRunner>(devicetoolPath().toStdWString(), true);
    }
    elevated_ = runner_->started();
}

DevicetoolController::DevicetoolController(std::unique_ptr<DevicetoolRunner> runner, QObject* parent)
    : QObject(parent), runner_(std::move(runner)) {
    elevated_ = runner_->started();
}

DevicetoolController::~DevicetoolController() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    runner_->cancel();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool DevicetoolController::working() const {
    return phase_ == QLatin1String("uac") || phase_ == QLatin1String("running") || phase_ == QLatin1String("restarting") ||
           phase_ == QLatin1String("testing");
}

void DevicetoolController::post(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    if (!worker_.joinable()) worker_ = std::thread([this] { loop(); });
    wake_.notify_one();
}

void DevicetoolController::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

void DevicetoolController::publish(std::function<void(DevicetoolController*)> change) {
    QMetaObject::invokeMethod(
        this,
        [this, change = std::move(change)] {
            change(this);
            emit changed();
        },
        Qt::QueuedConnection);
}

void DevicetoolController::start(const Request& request) {
    if (working()) return;
    last_ = request;
    kind_ = request.kind;
    target_ = request.guid;
    reason_.clear();
    restarted_ = false;
    row_status_.clear();
    bool changes = request.kind != QLatin1String("test");
    if (request.kind == QLatin1String("apply")) {
        changes = false;
        for (const QVariant& p : request.plans) {
            const QVariantMap plan = p.toMap();
            row_status_.insert(plan.value(QStringLiteral("guid")).toString(), QStringLiteral("queued"));
            changes |= !plan.value(QStringLiteral("commands")).toList().isEmpty();
        }
    }
    // Set now, so a second click before the worker starts finds it working.
    phase_ = changes && !runner_->started() ? QStringLiteral("uac") : QStringLiteral("running");
    emit changed();
    post([this, request, changes] {
        if (request.kind == QLatin1String("apply")) {
            applyPlans(request, changes);
        } else {
            operate(request);
        }
    });
}

void DevicetoolController::run(const QString& kind, const QString& guid, const QStringList& args) {
    if (args.isEmpty()) return;
    start({kind, guid, args, {}});
}

void DevicetoolController::requestApproval() { start({QStringLiteral("approval"), {}, {}, {}}); }

void DevicetoolController::apply(const QVariantList& plans) { start({QStringLiteral("apply"), {}, {}, plans}); }

void DevicetoolController::retry() {
    if (!last_.kind.isEmpty()) start(last_);
}

void DevicetoolController::clear() {
    if (working() || phase_.isEmpty()) return;
    phase_.clear();
    reason_.clear();
    row_status_.clear();
    emit changed();
}

void DevicetoolController::copyDetails() {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) clipboard->setText(details_);
}

bool DevicetoolController::loadScript(const QString& script) {
    if (working()) return false;
    auto scripted = std::make_unique<ScriptedRunner>();
    if (!scripted->load(script.toUtf8())) return false;
    // Nothing is working, so the worker holds no job that uses the old runner.
    {
        std::lock_guard lock(mutex_);
        runner_ = std::move(scripted);
    }
    last_ = {};
    phase_.clear();
    kind_.clear();
    target_.clear();
    reason_.clear();
    row_status_.clear();
    elevated_ = runner_->started();
    emit changed();
    return true;
}

// ---------------------------------------------------------------------------
// On the worker

bool DevicetoolController::approve() {
    if (runner_->started()) return true;
    publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("uac"); });
    const DWORD error = runner_->start();
    if (error == ERROR_CANCELLED) {
        publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("declined"); });
        return false;
    }
    if (error != ERROR_SUCCESS) {
        const QString reason = windowsMessage(error);
        publish([reason](DevicetoolController* c) {
            c->phase_ = QStringLiteral("failed");
            c->reason_ = reason;
            c->details_ = QStringLiteral("isotone-devicetool serve\n") + reason;
        });
        return false;
    }
    publish([](DevicetoolController* c) { c->elevated_ = true; });
    return true;
}

DevicetoolController::Outcome DevicetoolController::command(const std::vector<std::wstring>& args, bool direct,
                                                            DevicetoolResult* result, QString* reason) {
    const DevicetoolResult r = direct ? runner_->runDirect(args) : runner_->run(args);
    const QString details = QStringLiteral("isotone-devicetool %1\n%2\n%3")
                                .arg(joined(args),
                                     r.error != ERROR_SUCCESS ? windowsMessage(r.error) : QStringLiteral("exit %1").arg(r.exit_code),
                                     QString::fromStdString(r.json));
    const bool session_lost = !direct && !runner_->started();
    publish([details, session_lost](DevicetoolController* c) {
        c->details_ = details;
        if (session_lost) c->elevated_ = false;
    });
    if (result) *result = r;
    if (r.error == ERROR_SUCCESS && r.exit_code == 0) return Outcome::ok;
    if (r.error == ERROR_SUCCESS && r.exit_code == 4) {
        *reason = QStringLiteral("Another install is running.");
        return Outcome::busy;
    }
    *reason = reason_of(r);
    return Outcome::failed;
}

void DevicetoolController::fail(Outcome outcome, const QString& reason) {
    publish([outcome, reason](DevicetoolController* c) {
        c->phase_ = outcome == Outcome::busy ? QStringLiteral("busy") : QStringLiteral("failed");
        c->reason_ = outcome == Outcome::busy ? QString() : reason;
    });
}

DevicetoolController::Outcome DevicetoolController::restartAndTest(const QStringList& tests, QString* reason, QString* failed_test) {
    publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("restarting"); });
    QString ignored;
    if (command({L"restart-audio"}, false, nullptr, &ignored) != Outcome::ok) return Outcome::restart_failed;
    publish([](DevicetoolController* c) {
        c->restarted_ = true;
        c->phase_ = QStringLiteral("testing");
    });
    for (const QString& guid : tests) {
        if (command({L"test", guid.toStdWString()}, true, nullptr, reason) != Outcome::ok) {
            *failed_test = guid;
            return Outcome::failed;
        }
    }
    return Outcome::ok;
}

void DevicetoolController::operate(const Request& request) {
    const bool test_only = request.kind == QLatin1String("test");
    if (!test_only && !approve()) return;
    if (request.kind == QLatin1String("approval")) {
        publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("done"); });
        return;
    }
    publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("running"); });
    const auto finished = [this, request] {
        publish([request](DevicetoolController* c) { emit c->finished(request.kind, request.guid); });
    };
    DevicetoolResult result;
    QString reason;
    const Outcome outcome = command(wide(request.args), test_only, &result, &reason);
    if (outcome != Outcome::ok) {
        fail(outcome, reason);
        finished();
        return;
    }
    if (test_only) {
        publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("done"); });
        finished();
        return;
    }

    // The outputs changed: the operation's own, and each one repair reattached.
    QStringList tests;
    if (!request.guid.isEmpty()) tests << request.guid;
    const QJsonObject json = QJsonDocument::fromJson(QByteArray::fromStdString(result.json)).object();
    for (const QJsonValue& d : json.value(QStringLiteral("repaired")).toArray()) {
        const QJsonObject o = d.toObject();
        const QString guid = o.value(QStringLiteral("guid")).toString();
        if (o.value(QStringLiteral("ok")).toBool() && o.value(QStringLiteral("mode")).isString() && !tests.contains(guid))
            tests << guid;
    }
    QString failed_test;
    const Outcome after = restartAndTest(tests, &reason, &failed_test);
    if (after == Outcome::restart_failed) {
        publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("reboot"); });
    } else if (after == Outcome::failed) {
        publish([reason](DevicetoolController* c) {
            c->kind_ = QStringLiteral("test");
            c->phase_ = QStringLiteral("failed");
            c->reason_ = reason;
        });
    } else {
        publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("done"); });
    }
    finished();
}

void DevicetoolController::applyPlans(const Request& request, bool changes) {
    if (changes && !approve()) return;
    publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("running"); });
    const auto row = [this](const QString& guid, const QString& status) {
        publish([guid, status](DevicetoolController* c) { c->row_status_.insert(guid, status); });
    };
    const std::filesystem::path dir = equalizerApoConfigDir();
    QString first_failure;
    QStringList tests;
    bool changed = false;
    for (const QVariant& p : request.plans) {
        const QVariantMap plan = p.toMap();
        const QString guid = plan.value(QStringLiteral("guid")).toString();
        QString reason;
        bool ok = true;
        bool ran = false;
        for (const QVariant& c : plan.value(QStringLiteral("commands")).toList()) {
            const QStringList args = c.toStringList();
            row(guid, QString::fromLatin1(verb_of(args)));
            const Outcome outcome = command(wide(args), false, nullptr, &reason);
            if (outcome != Outcome::ok) {
                ok = false;
                break;
            }
            ran = changed = true;
        }
        if (ok && plan.value(QStringLiteral("attach")).toBool()) {
            row(guid, QStringLiteral("attaching"));
            const isotone::ui::AttachOutcome a = isotone::ui::attach_config(dir, false);
            if (a.error != ERROR_SUCCESS) {
                ok = false;
                reason = windowsMessage(a.error);
            }
        }
        if (ok && plan.value(QStringLiteral("removeBlock")).toBool()) {
            row(guid, QStringLiteral("removing"));
            isotone::compat::CompatWriter writer(dir);
            DWORD error = writer.load();
            if (error == ERROR_SUCCESS) error = writer.remove(guid.toStdString());
            if (error != ERROR_SUCCESS) {
                ok = false;
                reason = windowsMessage(error);
            }
        }
        if (ran) tests << guid;
        if (!ok && first_failure.isEmpty()) first_failure = reason;
        row(guid, ok ? plan.value(QStringLiteral("result")).toString() : QStringLiteral("failed"));
    }

    if (changed) {
        QString reason, failed_test;
        const Outcome after = restartAndTest(tests, &reason, &failed_test);
        if (after == Outcome::restart_failed) {
            publish([](DevicetoolController* c) { c->phase_ = QStringLiteral("reboot"); });
            publish([](DevicetoolController* c) { emit c->finished(QStringLiteral("apply"), QString()); });
            return;
        }
        if (after == Outcome::failed) {
            row(failed_test, QStringLiteral("failed"));
            if (first_failure.isEmpty()) first_failure = reason;
        }
    }
    publish([first_failure](DevicetoolController* c) {
        c->phase_ = first_failure.isEmpty() ? QStringLiteral("done") : QStringLiteral("failed");
        c->reason_ = first_failure;
        emit c->finished(QStringLiteral("apply"), QString());
    });
}
