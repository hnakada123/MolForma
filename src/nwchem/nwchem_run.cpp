// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_run.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/execution_environment.h"
#include "nwchem/history.h"
#include "nwchem/remote_run.h"
#include "nwchem/remote_supervisor.h"
#include "nwchem/run_process.h"
#include "nwchem/run_record.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace mf {
using namespace execution_detail;
NwchemRun::NwchemRun(QObject *parent) : QObject(parent) {
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] {
        if (::setsid() == -1)
            ::_exit(126);
    });
#endif
    timer.setInterval(250);
    killTimer.setSingleShot(true);
    killTimer.setInterval(2000);
    connect(&killTimer, &QTimer::timeout, this, [this] { signalProcess(true); });
    connect(&timer, &QTimer::timeout, this, [this] {
        if (watching) {
            poll();
            return;
        }
        if (status != RunState::Stopping && current.timeoutSeconds &&
            elapsedSeconds() >= current.timeoutSeconds) {
            timedOut = true;
            stop();
        }
        emit changed();
    });
    connect(&process, &QProcess::started, this, [this] {
        groupId = process.processId();
        const auto identity = executionProcessIdentity(groupId);
        for (auto it = identity.begin(); it != identity.end(); ++it)
            current.manifest[it.key()] = it.value();
        if (status == RunState::Stopping) {
            signalProcess(false);
            killTimer.start();
        } else
            status = RunState::Running;
        record();
        emit changed();
    });
    connect(&process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus state) { finish(code, state); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1, QProcess::CrashExit, process.errorString());
    });
}
NwchemRun::~NwchemRun() {
    shutdown();
}
qint64 NwchemRun::elapsedSeconds() const {
    if (!active())
        return duration;
    // A run that another session started has only its record to tell the time by.
    if (const auto started = RunRecord(current.manifest).started(); watching && started.isValid())
        return std::max<qint64>(0, started.secsTo(QDateTime::currentDateTimeUtc()));
    return elapsed.elapsed() / 1000;
}
void NwchemRun::start(const Document &d, const QString &base) {
    if (active())
        fail(tr("計算は既に実行中です。"));
    // The Preferences as they are when the run begins: checked and saved with this one state of them.
    startPrepared(saveExecutionBundle(d, base, true, executionEnvironment()));
}
void NwchemRun::startPrepared(const ExecutionPlan &plan) {
    if (active())
        fail(tr("計算は既に実行中です。"));
    current = plan;
    status = RunState::Starting;
    detail.clear();
    timedOut = false;
    watching = false;
    groupId = 0;
    duration = 0;
    resumed = 0;
    elapsed.start();
    if (!supervisor.isEmpty()) {
        launch();
        return;
    }
    // Only a supervisor copies a job to its destination and follows it there.
    if (remoteRun(current.manifest)) {
        record();
        finish(-1, QProcess::CrashExit,
               tr("リモート実行には、計算を見張るプロセスが必要です（MCPでは --mcp-keep-runs "
                  "で起動してください）。"));
        return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    for (auto it = current.environment.begin(); it != current.environment.end(); ++it)
        environment.insert(it.key(), it.value());
    process.setProgram(current.program);
    process.setArguments(current.arguments);
    process.setWorkingDirectory(current.directory);
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(jobFile(current.directory, JobFile::output));
    process.setStandardErrorFile(jobFile(current.directory, JobFile::errors));
    current.manifest["started_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record();
    process.start();
    if (active())
        timer.start();
    emit changed();
}
void NwchemRun::signalProcess(bool force) {
    // A forked child still inherits the application's signal handlers until exec.
    // Defer graceful cancellation to started(); a hung startup can still be killed.
    if (!active() || (!force && process.state() != QProcess::Running))
        return;
    signalGroup(groupId, force);
    if (force)
        process.kill();
    else
        process.terminate();
}
void NwchemRun::launch() {
    current.manifest["started_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QFile::remove(stopRequest(current.directory));
    record();
    // The supervisor must not hold the streams of this process (an MCP client reads them).
    QProcess starter;
    starter.setProgram(supervisor);
    starter.setArguments({"--run-job", current.directory});
    starter.setWorkingDirectory(current.directory);
    starter.setStandardInputFile(QProcess::nullDevice());
    starter.setStandardOutputFile(QProcess::nullDevice());
    starter.setStandardErrorFile(QProcess::nullDevice());
    supervisorPid = 0;
    if (!starter.startDetached(&supervisorPid)) {
        finish(-1, QProcess::CrashExit, tr("計算を実行するプロセスを起動できません: %1").arg(supervisor));
        return;
    }
    watching = true;
    unreadable = 0;
    timer.start();
    emit changed();
}
bool NwchemRun::attach(const QString &directory) {
    if (active())
        fail(tr("計算は既に実行中です。"));
    const auto job = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    QJsonObject manifest;
    try {
        manifest = readRunRecord(job, RecordCheck::Json);
    } catch (const std::exception &) {
        return false;
    }
    const auto state = RunRecord(manifest).state();
    if (!RunRecord(manifest).hasFormat() || !runStateActive(state))
        return false;
    // A remote run whose supervisor ended (the computer restarted) gets another one, which looks at the
    // job there again.
    qint64 restarted = 0;
    if (!runSupervised(manifest) &&
        (!remoteRun(manifest) || supervisor.isEmpty() || !resumeRemoteRun(job, supervisor, &restarted)))
        return false;
    if (restarted)
        manifest.remove("supervisor");
    current = runRecordPlan(job, manifest);
    status = state;
    detail = RunRecord(manifest).error();
    timedOut = false;
    watching = true;
    supervisorPid = restarted;
    unreadable = resumed = 0;
    groupId = duration = 0;
    timer.start();
    emit changed();
    return true;
}
void NwchemRun::poll() {
    QJsonObject manifest;
    try {
        manifest = readRunRecord(current.directory, RecordCheck::Json);
    } catch (const std::exception &) {
        manifest = {};
    }
    auto state = RunRecord(manifest).state();
    if (manifest.isEmpty() || state == RunState::Invalid) {
        // The record is replaced in one step, so this is no half-written file: the folder is gone.
        if (++unreadable < 20)
            return;
        manifest = current.manifest;
        state = RunState::Interrupted;
        manifest["error"] = tr("実行記録を読めなくなりました。");
    }
    unreadable = 0;
    // Before the supervisor has written its own identity, the process this object started stands
    // for it: a supervisor that ended at once left a record without any process.
    if (runStateActive(state) && !manifest.contains("supervisor") && supervisorPid > 0 &&
        executionProcessGone(executionProcessIdentity(supervisorPid))) {
        try {
            manifest = readRunRecord(current.directory, RecordCheck::Json);
            if (runStateActive(RunRecord(manifest).state()) && !manifest.contains("supervisor")) {
                manifest["supervisor"] = executionProcessIdentity(supervisorPid);
                writeRunRecord(current.directory, manifest);
            }
        } catch (const std::exception &) {
        }
        state = RunRecord(manifest).state();
    }
    if (runStateActive(state) && remoteRun(manifest) && executionRunGone(manifest)) {
        // The supervisor of a remote run ended (it was killed, the computer restarted) while the job may
        // go on there: another supervisor takes it up. After a few in a row, the history offers it.
        qint64 restarted = 0;
        if (!supervisor.isEmpty() && resumed < 3 &&
            resumeRemoteRun(current.directory, supervisor, &restarted)) {
            ++resumed;
            supervisorPid = restarted;
            manifest.remove("supervisor");
            current.manifest = manifest;
            emit changed();
            return;
        }
        duration = elapsedSeconds();
        timer.stop();
        watching = false;
        current.manifest = manifest;
        status = RunState::Interrupted;
        detail = QT_TR_NOOP("リモートの計算を見張るプロセスが終了しました。「履歴」で再接続できます。");
        emit changed();
        return;
    }
    if (runStateActive(state) && executionRunGone(manifest)) {
        // The supervisor ended without recording the end, and NWChem is not running either. Its last
        // word may have come between the reading and the look at the processes: the repair reads again.
        try {
            repairHistoryEntry(current.directory);
        } catch (const std::exception &) {
        }
        try {
            manifest = readRunRecord(current.directory, RecordCheck::Json);
        } catch (const std::exception &) {
        }
        state = RunRecord(manifest).state();
        if (state == RunState::Invalid)
            state = RunState::Interrupted;
    }
    // The stop this object asked for shows before the supervisor has recorded it.
    const auto shown = status == RunState::Stopping && runStateActive(state) ? RunState::Stopping : state;
    current.manifest = manifest;
    status = shown;
    detail = RunRecord(manifest).error();
    if (!runStateActive(state)) {
        timer.stop();
        watching = false;
        duration = qint64(manifest["elapsed_seconds"].toDouble());
        QFile::remove(stopRequest(current.directory));
    }
    emit changed(); // Also for the time that passed.
}
void NwchemRun::stop() {
    if (status != RunState::Starting && status != RunState::Running)
        return;
    status = RunState::Stopping;
    if (watching) {
        // The supervisor finds the request when it next looks, even one that has only just been started.
        QFile request(stopRequest(current.directory));
        const bool asked = request.open(QIODevice::WriteOnly);
        const auto keeper = current.manifest["supervisor"].toObject();
        const bool gone = keeper.isEmpty() ? supervisorPid > 0 &&
                                                 executionProcessGone(executionProcessIdentity(supervisorPid))
                                           : executionProcessGone(keeper);
        // No supervisor to ask: the processes it left are ended here.
        if (!asked || gone)
            signalGroup(current.manifest["pid"].toInteger(), true);
        emit changed();
        return;
    }
    signalProcess(false);
    killTimer.start();
    record();
    emit changed();
}
void NwchemRun::release() {
    if (!watching)
        return;
    timer.stop();
    watching = false;
    status = RunState::Idle;
}
void NwchemRun::finish(int code, QProcess::ExitStatus exitStatus, const QString &error) {
    if (!active())
        return;
    // Clean up surviving children in our private process group before allowing a new run.
    signalProcess(true);
    duration = elapsed.elapsed() / 1000;
    groupId = 0;
    timer.stop();
    killTimer.stop();
    status = timedOut                                                              ? RunState::TimedOut
             : status == RunState::Stopping                                        ? RunState::Cancelled
             : !error.isEmpty() || exitStatus != QProcess::NormalExit || code != 0 ? RunState::Failed
                                                                                   : RunState::Finished;
    detail = error;
    recordRunEnd(current.manifest, current.directory,
                 {.state = status,
                  .exitCode = code,
                  .normalExit = exitStatus == QProcess::NormalExit,
                  .finished = QDateTime::currentDateTimeUtc(),
                  .elapsedSeconds = duration,
                  .error = error,
                  .launchError = error});
    record();
    emit changed();
}
void NwchemRun::record() {
    current.manifest["status"] = runStateKey(status);
    current.manifest["elapsed_seconds"] = elapsedSeconds();
    current.manifest["error"] = detail;
    try {
        writeRunRecord(current.directory, current.manifest);
    } catch (const std::exception &e) {
        detail = QString::fromUtf8(e.what());
    }
}
void NwchemRun::shutdown() {
    if (!active())
        return;
    stop();
    if (watching) {
        // The supervisor asks NWChem to end, then ends it after two seconds.
        QElapsedTimer waited;
        waited.start();
        while (watching && waited.elapsed() < 8000) {
            QThread::msleep(100);
            poll();
        }
        return;
    }
    if (!process.waitForFinished(2000)) {
        signalProcess(true);
        process.waitForFinished(2000);
    }
}
} // namespace mf
