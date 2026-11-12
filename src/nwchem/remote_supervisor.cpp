// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_supervisor.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/execution_environment.h"
#include "nwchem/remote_host.h"
#include "nwchem/remote_run.h"
#include "nwchem/remote_shell.h"
#include "nwchem/run_process.h"
#include "nwchem/run_record.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QThread>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace mf {
using namespace execution_detail;
namespace {
// Files of a job folder that stay on this computer: the record, the project, the user's note and what
// asks the supervisor to stop. The output and errors come back from the destination.
const QStringList localFiles{JobFile::record,         JobFile::project, JobFile::memo,   JobFile::stopRequest,
                             JobFile::supervisorLock, JobFile::output,  JobFile::errors, ".retrieved"};
// What the copy that comes back does not replace: the record and inputs of this computer.
const QStringList keptFiles{JobFile::record,         JobFile::project, JobFile::memo,  JobFile::stopRequest,
                            JobFile::supervisorLock, JobFile::input,   JobFile::script};
const QRegularExpression &jobId() {
    static const QRegularExpression pattern("^[A-Za-z0-9._\\[\\]-]{1,128}$");
    return pattern;
}
#ifdef Q_OS_UNIX
volatile std::sig_atomic_t remoteSignalled = 0;
void remoteSignal(int) {
    remoteSignalled = 1;
}
#else
constexpr int remoteSignalled = 0;
#endif

// What a look at a running job found.
struct Look {
    bool reached = false, ended = false, missing = false;
    std::optional<int> exit;
    QString state, scheduler, accounting;
    QByteArray log;  // The output there from `from` on.
    qint64 from = 0; // Past the offset asked for when the output grew by more than one look takes.
};
// At most this much of the output comes with one look; the rest comes back with the job folder.
constexpr qint64 logChunk = 4 * 1024 * 1024;

// The supervisor of one remote run (see remoteJobMain).
class RemoteSupervisor {
  public:
    // registered: the destination of that name in the user's Preferences as the supervisor starts.
    RemoteSupervisor(QString job, QJsonObject record, const ExecutionEnvironment &environment)
        : directory(std::move(job)), manifest(std::move(record)) {
        connection.host = RemoteHost::fromJson(remote()["host"].toObject());
        registered = environment.remote(connection.host.name);
    }
    int exec();

  private:
    QString directory;
    QJsonObject manifest;
    Connection connection;
    std::optional<RemoteHost> registered;
    RunState status = RunState::Starting;
    bool cancelSent = false, timedOut = false;
    int unwritable = 0, cancelFailures = 0;
    QJsonObject remote() const {
        return manifest["remote"].toObject();
    }
    void set(const QString &key, const QJsonValue &value) {
        auto r = remote();
        if (value.isUndefined() || value.isNull())
            r.remove(key);
        else
            r[key] = value;
        manifest["remote"] = r;
    }
    QString path(const QString &name) const {
        return QDir(directory).filePath(name);
    }
    bool stopAsked() const {
        return QFileInfo::exists(path(JobFile::stopRequest));
    }
    qint64 elapsed() const {
        const auto started = RunRecord(manifest).started();
        return started.isValid() ? std::max<qint64>(0, started.secsTo(QDateTime::currentDateTimeUtc())) : 0;
    }
    void save();
    // A look that did not reach the destination: why, kept in the record until a look does.
    void unreachable(const char *message, const QString &detail) {
        set("message", QString::fromUtf8(message));
        set("detail", detail);
        if (!remote().contains("failing_since"))
            set("failing_since", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        save();
    }
    void reached() {
        set("message", {});
        set("detail", {});
        set("failing_since", {});
    }
    // Waits for the seconds, watching for a stop request. False when a signal ends the supervisor.
    bool wait(int seconds) const {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < seconds * 1000LL) {
            if (remoteSignalled)
                return false;
            if (!cancelSent && stopAsked())
                return true;
            QThread::msleep(200);
        }
        return !remoteSignalled;
    }
    int end(RunState state, const char *message = nullptr, const QString &detail = {},
            std::optional<int> exitCode = std::nullopt);
    bool upload();
    bool submit();
    Look look();
    bool cancel();
    bool retrieve();
};
void RemoteSupervisor::save() {
    manifest["status"] = runStateKey(status);
    manifest["elapsed_seconds"] = elapsed();
    try {
        writeRunRecord(directory, manifest);
        unwritable = 0;
    } catch (const std::exception &) {
        ++unwritable;
    }
}
int RemoteSupervisor::end(RunState state, const char *message, const QString &detail,
                          std::optional<int> exitCode) {
    status = state;
    set("detail", detail.isEmpty() ? QJsonValue() : QJsonValue(detail));
    set("message", {});
    set("failing_since", {});
    set("phase", "done");
    for (const auto *name : {JobFile::output, JobFile::errors})
        if (!QFileInfo(path(name)).isFile())
            writeFile(path(name), {});
    // A job that never ran failed to start; the rest is for the log to tell.
    const bool started = remote().contains("job");
    recordRunEnd(manifest, directory,
                 {.state = state,
                  .exitCode = exitCode ? *exitCode : -1,
                  .normalExit = exitCode.has_value(),
                  .finished = QDateTime::currentDateTimeUtc(),
                  .elapsedSeconds = elapsed(),
                  // Kept in Japanese whatever the language: the run panel and the history show it through
                  // translated().
                  .error = message ? QString::fromUtf8(message) : QString(),
                  .launchError = !started && message ? translated(QString::fromUtf8(message)) : QString(),
                  // A run that did not start there points at its destination, not at the programs of this PC.
                  .launchSetting = "remote"});
    save();
    QFile::remove(path(JobFile::stopRequest));
    return 0;
}
bool RemoteSupervisor::upload() {
    status = RunState::Starting;
    set("phase", "upload");
    save();
    for (const auto *name : {JobFile::output, JobFile::errors})
        if (!QFileInfo::exists(path(name)))
            writeFile(path(name), {});
    const auto job = QFileInfo(directory).fileName();
    if (!QRegularExpression("^[A-Za-z0-9._-]+$").match(job).hasMatch()) {
        end(RunState::Failed, QT_TR_NOOP("ジョブフォルダーの名前に、実行先へ送れない文字があります。"), job);
        return false;
    }
    // The folder there, as an absolute path for scp.
    QString target;
    for (int attempt = 0; target.isEmpty(); ++attempt) {
        if (stopAsked()) {
            end(RunState::Cancelled);
            return false;
        }
        const auto nonce = newNonce();
        const auto reply = connection.run("root=" + remoteRoot(connection.host.directory) + "\n" +
                                          "mkdir -p -- \"$root\" && cd -- \"$root\" && mkdir -p -- " +
                                          quote(job) + " && cd -- " + quote(job) + " || exit 1\n" +
                                          scriptStart(nonce) + "pwd -P\n" + scriptEnd(nonce));
        const auto text = answer(reply.out, nonce);
        if (text) {
            target = QString::fromUtf8(*text).trimmed();
            if (!remotePath().match(target).hasMatch()) {
                end(RunState::Failed, QT_TR_NOOP("実行先の作業フォルダーのパスに、使えない文字があります。"),
                    target);
                return false;
            }
            break;
        }
        if (reply.code != 255 || attempt >= 2) {
            end(RunState::Failed, QT_TR_NOOP("実行先にジョブフォルダーを作れません。"), reply.detail());
            return false;
        }
        unreachable(QT_TR_NOOP("実行先へ接続できません。"), reply.detail());
        if (!wait(10))
            return false;
    }
    set("directory", target);
    reached();
    save();
    QStringList sources;
    for (const auto &info :
         QDir(directory).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        if (!info.isSymLink() && !localFiles.contains(info.fileName()))
            sources << info.absoluteFilePath();
    for (int attempt = 0;; ++attempt) {
        const auto reply =
            connection.copy(sources, connection.remote(target + '/'), [this] { return stopAsked(); });
        if (reply.cancelled) {
            end(RunState::Cancelled);
            return false;
        }
        if (reply.code == 0 && reply.failure.isEmpty())
            return true;
        if (attempt >= 2) {
            end(RunState::Failed, QT_TR_NOOP("ジョブを実行先へ複写できません。"), reply.detail());
            return false;
        }
        unreachable(QT_TR_NOOP("ジョブを実行先へ複写できません。"), reply.detail());
        if (!wait(10))
            return false;
    }
}
bool RemoteSupervisor::submit() {
    set("phase", "submit");
    save();
    const auto scheduler = connection.host.scheduler;
    QString command;
    if (scheduler == "slurm")
        command = "id=$(sbatch --parsable job.sh) || exit 3\n";
    else if (scheduler == "pbs")
        command = "id=$(qsub job.sh) || exit 3\n";
    else
        // In a session of its own (setsid), so that a stop reaches the launcher and NWChem as well.
        command = "if command -v setsid >/dev/null 2>&1; then\n"
                  "  setsid nohup ./job.sh > job.log 2>&1 < /dev/null &\n"
                  "else\n"
                  "  nohup ./job.sh > job.log 2>&1 < /dev/null &\n"
                  "fi\n"
                  "id=$!\n";
    for (int attempt = 0;; ++attempt) {
        if (stopAsked()) {
            end(RunState::Cancelled);
            return false;
        }
        const auto nonce = newNonce();
        const auto reply = connection.run("cd -- " + quote(remote()["directory"].toString()) +
                                          " || exit 3\nrm -f exit_code\nchmod u+x job.sh\n" + command +
                                          scriptStart(nonce) + "printf '%s\\n' \"$id\"\n" + scriptEnd(nonce));
        if (const auto text = answer(reply.out, nonce)) {
            auto id = QString::fromUtf8(*text).trimmed();
            if (scheduler == "slurm")
                id = id.section(';', 0, 0); // "<job>;<cluster>"
            if (!jobId().match(id).hasMatch()) {
                end(RunState::Failed, QT_TR_NOOP("スケジューラーの返したジョブIDを読めません。"),
                    id.left(200));
                return false;
            }
            set("job", id);
            set("phase", "submitted");
            set("state", scheduler == "shell" ? "running" : "queued");
            set("submitted_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            reached();
            status = RunState::Running;
            save();
            return true;
        }
        // ssh itself failed (255): nothing ran there, so the job can be submitted again.
        if (reply.code != 255 || attempt >= 2) {
            end(RunState::Failed, QT_TR_NOOP("ジョブを投入できません。"), reply.detail());
            return false;
        }
        unreachable(QT_TR_NOOP("実行先へ接続できません。"), reply.detail());
        if (!wait(10))
            return false;
    }
}
Look RemoteSupervisor::look() {
    Look result;
    const auto scheduler = connection.host.scheduler;
    const auto id = remote()["job"].toString();
    const auto nonce = newNonce();
    QString query;
    if (scheduler == "slurm")
        query = "q=$(squeue -h -j " + id + " -o %T 2>&1); r=$?\n" +
                "printf 'queue %s %s\\n' \"$r\" \"$(printf '%s' \"$q\" | tr '\\n' ' ')\"\n"
                "case \"$q\" in ''|*nvalid*) printf 'account %s\\n' \"$(sacct -n -X -P -j " +
                id + " -o State 2>/dev/null | head -n 1)\";; esac\n";
    else if (scheduler == "pbs")
        query = "q=$(qstat -f " + id + " 2>&1); r=$?\n" +
                "printf 'queue %s %s\\n' \"$r\" \"$(printf '%s' \"$q\" | grep -i -E "
                "'job_state|exit_status|unknown|finished' | tr '\\n' ' ')\"\n";
    else
        query = "if kill -0 " + id + " 2>/dev/null && ps -p " + id +
                " -o args= 2>/dev/null | grep -q 'job[.]sh'; then printf 'alive\\n'; fi\n";
    // How much of the output there has come here so far.
    const auto offset = remote().contains("log_offset")
                            ? qint64(remote()["log_offset"].toDouble())
                            : std::max<qint64>(0, QFileInfo(path(JobFile::output)).size());
    const auto reply = connection.run(
        scriptStart(nonce) + "if ! cd -- " + quote(remote()["directory"].toString()) +
        " 2>/dev/null; then printf 'missing\\n%s\\n' " + quote(nonce + "-LOG") + "; " +
        scriptEnd(nonce).trimmed() + "; exit 0; fi\n" +
        "if [ -f exit_code ]; then printf 'exit %s\\n' \"$(cat exit_code)\"; fi\n" + query + "from=" +
        QString::number(offset) + "\nif [ -f output.out ]; then\n  size=$(wc -c < output.out | tr -d ' ')\n" +
        "  if [ \"$((size - from))\" -gt " + QString::number(logChunk) + " ]; then from=$((size - " +
        QString::number(logChunk) + ")); fi\nfi\nprintf 'from %s\\n' \"$from\"\n" + "printf '%s\\n' " +
        quote(nonce + "-LOG") + "\n" + "if [ -f output.out ]; then tail -c +$((from + 1)) output.out; fi\n" +
        scriptEnd(nonce));
    const auto text = answer(reply.out, nonce);
    if (!text) {
        unreachable(QT_TR_NOOP("実行先の状態を確認できません。"), reply.detail());
        return result;
    }
    const auto split = text->indexOf((nonce + "-LOG\n").toLatin1());
    if (split < 0) {
        unreachable(QT_TR_NOOP("実行先の状態を確認できません。"), reply.detail());
        return result;
    }
    result.reached = true;
    result.log = text->mid(split + nonce.size() + 5);
    bool alive = false, queued = false, gone = false;
    for (const auto &raw : QString::fromUtf8(text->left(split)).split('\n', Qt::SkipEmptyParts)) {
        const auto line = raw.trimmed();
        const auto word = line.section(' ', 0, 0), rest = line.section(' ', 1).trimmed();
        if (word == "missing")
            result.missing = true;
        else if (word == "from")
            result.from = std::max<qint64>(0, rest.toLongLong());
        else if (word == "exit") {
            bool ok = false;
            const int code = rest.toInt(&ok);
            if (ok)
                result.exit = code;
        } else if (word == "alive")
            alive = true;
        else if (word == "account")
            result.accounting = rest.section(' ', 0, 0).section('|', 0, 0).toUpper();
        else if (word == "queue") {
            const auto code = rest.section(' ', 0, 0).toInt();
            const auto said = rest.section(' ', 1).trimmed();
            result.scheduler = said.left(200);
            if (scheduler == "slurm") {
                static const QStringList over{"COMPLETED",     "FAILED",      "CANCELLED", "TIMEOUT",
                                              "NODE_FAIL",     "BOOT_FAIL",   "DEADLINE",  "PREEMPTED",
                                              "OUT_OF_MEMORY", "SPECIAL_EXIT"};
                const auto state = said.section(' ', 0, 0).toUpper();
                if (code == 0 && state.isEmpty())
                    gone = true;
                else if (code != 0 && said.contains("nvalid job id", Qt::CaseInsensitive))
                    gone = true;
                else if (code == 0) {
                    gone = over.contains(state);
                    queued = true;
                    result.state = state == "PENDING" || state == "CONFIGURING" ? "queued"
                                   : state == "RUNNING"                         ? "running"
                                   : state == "COMPLETING"                      ? "ending"
                                   : state == "SUSPENDED" || state == "STOPPED" ? "held"
                                   : gone                                       ? "ended"
                                                                                : "unknown";
                }
            } else {
                static const QRegularExpression stateLine("job_state\\s*=\\s*([A-Za-z])");
                const auto match = stateLine.match(said);
                if (code == 0 && match.hasMatch()) {
                    const auto state = match.captured(1).toUpper();
                    gone = state == "F" || state == "C" || state == "X";
                    queued = true;
                    result.state = state == "Q" || state == "W" || state == "T"   ? "queued"
                                   : state == "R" || state == "B"                 ? "running"
                                   : state == "E"                                 ? "ending"
                                   : state == "H" || state == "S" || state == "U" ? "held"
                                   : gone                                         ? "ended"
                                                                                  : "unknown";
                } else if (code != 0 && (said.contains("unknown job", Qt::CaseInsensitive) ||
                                         said.contains("finished", Qt::CaseInsensitive)))
                    gone = true;
            }
        }
    }
    if (scheduler == "shell") {
        gone = !alive;
        result.state = alive ? "running" : "ended";
    } else if (gone)
        result.state = "ended";
    else if (!queued)
        result.state = "unknown"; // The scheduler did not answer: the job is not taken for ended.
    result.ended = result.missing || result.exit.has_value() || gone;
    reached();
    return result;
}
bool RemoteSupervisor::cancel() {
    const auto id = remote()["job"].toString();
    const auto scheduler = connection.host.scheduler;
    const QString command =
        scheduler == "slurm" ? "scancel " + id + " 2>&1 | grep -iv 'invalid job id' >&2; true\n"
        : scheduler == "pbs" ? "qdel " + id + " 2>&1 | grep -iv -E 'unknown job|finished' >&2; true\n"
                             : "kill -TERM -- -" + id + " 2>/dev/null || { pkill -TERM -P " + id +
                                   " 2>/dev/null; kill -TERM " + id + " 2>/dev/null; }\n";
    const auto nonce = newNonce();
    const auto reply = connection.run(command + scriptStart(nonce) + scriptEnd(nonce));
    if (answer(reply.out, nonce))
        return true;
    unreachable(QT_TR_NOOP("実行先のジョブを停止できません。"), reply.detail());
    return false;
}
bool RemoteSupervisor::retrieve() {
    const auto target = path(".retrieved");
    QDir(target).removeRecursively();
    const auto reply = connection.copy({connection.remote(remote()["directory"].toString())}, target,
                                       [this] { return stopAsked(); });
    if (reply.code != 0 || !reply.failure.isEmpty() || !QFileInfo(target).isDir()) {
        QDir(target).removeRecursively();
        if (!reply.cancelled)
            unreachable(QT_TR_NOOP("結果を実行先から回収できません。"), reply.detail());
        return false;
    }
    for (const auto &info :
         QDir(target).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const auto name = info.fileName();
        if (info.isSymLink() || keptFiles.contains(name) || name == ".retrieved")
            continue;
        const auto destination = path(name);
        const QFileInfo existing(destination);
        if (existing.isDir() && !existing.isSymLink())
            QDir(destination).removeRecursively();
        else if (existing.exists() || existing.isSymLink())
            QFile::remove(destination);
        QDir().rename(info.absoluteFilePath(), destination);
    }
    QDir(target).removeRecursively();
    reached();
    return true;
}
int RemoteSupervisor::exec() {
    // Only a destination of this user's Preferences, where it still leads: a record does not choose
    // where to connect.
    if (!registered || registered->host != connection.host.host || registered->port != connection.host.port)
        return end(RunState::Failed,
                   QT_TR_NOOP("実行先が環境設定にないか、接続先が変わっています。「3 "
                              "実行設定」で実行先を確認してください。"),
                   connection.host.name);
    if (connection.ssh.isEmpty() || connection.scp.isEmpty())
        return end(RunState::Failed, QT_TR_NOOP("このPCに ssh と scp が見つかりません。"));
    manifest["supervisor"] = executionProcessIdentity(QCoreApplication::applicationPid());
    status = RunRecord(manifest).state();
    // A stop that an earlier supervisor of the job already delivered.
    cancelSent = remote().contains("cancelled");
    timedOut = remote()["cancelled"].toString() == "time_limit";
    const auto phase = remote()["phase"].toString();
    if (phase.isEmpty() || phase == "prepared" || phase == "upload") {
        if (!upload() || !submit())
            return 0;
    } else if (phase == "submit")
        return end(RunState::Interrupted, QT_TR_NOOP("ジョブの投入中に見張り役のプロセスが終了したため、投入"
                                                     "されたか分かりません。実行先で確認してください。"));
    else {
        // What the record names there goes into commands: only what MolForma itself would have written.
        const auto id = remote()["job"].toString();
        if (!jobId().match(id).hasMatch() ||
            (connection.host.scheduler == "shell" && !QRegularExpression("^[0-9]+$").match(id).hasMatch()) ||
            !remotePath().match(remote()["directory"].toString()).hasMatch())
            return end(RunState::Failed, QT_TR_NOOP("実行記録にある実行先のジョブを読めません。"),
                       id.left(200));
        save(); // The supervisor that takes the job up from here.
    }
    // Follow the job until it ends there.
    const int timeout = manifest["timeout_seconds"].toInt();
    const bool shell = connection.host.scheduler == "shell";
    int unconfirmed = 0;
    Look last;
    if (remote()["phase"].toString() == "submitted") {
        for (;;) {
            if (unwritable > 3)
                return 1; // The job folder is gone.
            const bool asked = stopAsked() || (shell && timeout > 0 && elapsed() >= timeout);
            if (asked && !cancelSent) {
                timedOut = !stopAsked();
                status = RunState::Stopping;
                save();
                cancelSent = cancel();
                // Handled: from now on a request asks to stop waiting for the results instead.
                if (cancelSent) {
                    set("cancelled", timedOut ? QJsonValue("time_limit") : QJsonValue("stop"));
                    save();
                    QFile::remove(path(JobFile::stopRequest));
                } else {
                    if (++cancelFailures >= 3)
                        return end(
                            RunState::Interrupted,
                            QT_TR_NOOP("実行先のジョブを停止できませんでした。実行先で確認してください。"),
                            remote()["detail"].toString());
                }
            }
            last = look();
            if (last.reached) {
                // The log as far as it has come, for the run panel; the job folder brings the whole of it.
                const auto offset = remote().contains("log_offset")
                                        ? qint64(remote()["log_offset"].toDouble())
                                        : QFileInfo(path(JobFile::output)).size();
                if (!last.log.isEmpty() || last.from > offset) {
                    QFile log(path(JobFile::output));
                    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
                        if (last.from > offset)
                            log.write(
                                QString("\n… %1 bytes of the log are left out here until the job ends …\n")
                                    .arg(last.from - offset)
                                    .toUtf8());
                        log.write(last.log);
                    }
                    set("log_offset", double(std::max(offset, last.from) + last.log.size()));
                }
                set("state", last.state);
                set("scheduler_state", last.scheduler.isEmpty() ? QJsonValue() : QJsonValue(last.scheduler));
                set("checked_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                save();
                // A job that left no exit code is taken for ended at the second look only: the file may
                // reach a login node later than the job ends on its compute node.
                if (last.ended && (last.exit || last.missing || ++unconfirmed >= 2))
                    break;
                if (!last.ended)
                    unconfirmed = 0;
            }
            if (!wait(unconfirmed ? 5 : connection.host.interval))
                return 0; // Left to the next supervisor (resumeRemoteRun).
        }
        set("phase", "retrieve");
        set("state", "ended");
        set("exit", last.exit ? QJsonValue(*last.exit) : QJsonValue());
        set("accounting", last.accounting.isEmpty() ? QJsonValue() : QJsonValue(last.accounting));
        set("missing", last.missing ? QJsonValue(true) : QJsonValue());
        save();
    }
    const bool missing = remote()["missing"].toBool();
    // Copy the folder back, however long that takes to become possible.
    while (!missing && !retrieve()) {
        if (stopAsked() && !cancelSent)
            return end(RunState::Interrupted,
                       QT_TR_NOOP("結果を回収しないまま停止しました。実行先のフォルダーに残っています。"),
                       remote()["directory"].toString());
        if (!wait(connection.host.interval))
            return 0;
    }
    std::optional<int> code;
    if (remote()["exit"].isDouble())
        code = remote()["exit"].toInt();
    const auto accounting = remote()["accounting"].toString();
    if (code)
        return end(*code == 0 ? RunState::Finished : RunState::Failed, nullptr, {}, code);
    if (timedOut || accounting.startsWith("TIMEOUT"))
        return end(RunState::TimedOut);
    if (cancelSent || accounting.startsWith("CANCELLED"))
        return end(RunState::Cancelled);
    if (missing)
        return end(RunState::Interrupted, QT_TR_NOOP("実行先のジョブフォルダーがなくなりました。"),
                   remote()["directory"].toString());
    return end(
        RunState::Interrupted,
        QT_TR_NOOP(
            "実行先でジョブが終わりましたが、終了コードの記録がありません（スケジューラーによる停止など）。"),
        accounting.isEmpty() ? remote()["scheduler_state"].toString() : accounting);
}
} // namespace
int remoteJobMain(const QString &directory, QJsonObject manifest) {
    const auto state = RunRecord(manifest).state();
    const auto phase = manifest["remote"].toObject()["phase"].toString();
    QByteArray input;
    try {
        input = readFile(jobFile(directory, JobFile::input));
    } catch (const std::exception &) {
        return 2;
    }
    const bool fresh =
        state == RunState::Starting && (phase.isEmpty() || phase == "prepared") &&
        manifest["input_sha256"].toString() ==
            QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
    const bool resumed =
        runStateActive(state) && QStringList{"upload", "submit", "submitted", "retrieve"}.contains(phase);
    // A supervisor that is still there keeps its job.
    const auto keeper = manifest["supervisor"].toObject();
    if ((!fresh && !resumed) ||
        (!keeper.isEmpty() &&
         (keeper["host"].toString() != QSysInfo::machineHostName() || !executionProcessGone(keeper))))
        return 2;
    QLockFile lock(jobFile(directory, JobFile::supervisorLock));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 2;
#ifdef Q_OS_UNIX
    // Logging out or shutting down ends only this supervisor: the job goes on there, and the next
    // supervisor takes it up. Only a stop request (stop.request) cancels it.
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        std::signal(signal, remoteSignal);
#endif
    // The Preferences as they are when this supervisor takes the job up.
    RemoteSupervisor supervisor(directory, std::move(manifest), executionEnvironment());
    return supervisor.exec();
}
bool resumeRemoteRun(const QString &directory, const QString &program, qint64 *pid) {
    const auto job = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    QJsonObject manifest;
    try {
        manifest = readRunRecord(job, RecordCheck::Json);
    } catch (const std::exception &) {
        return false;
    }
    if (!RunRecord(manifest).hasFormat() || !remoteRun(manifest) ||
        !runStateActive(RunRecord(manifest).state()))
        return false;
    const auto keeper = manifest["supervisor"].toObject();
    if (!keeper.isEmpty() &&
        (keeper["host"].toString() != QSysInfo::machineHostName() || !executionProcessGone(keeper)))
        return false;
    // Until the new supervisor writes its own identity, the record names none that has ended.
    manifest.remove("supervisor");
    try {
        writeRunRecord(job, manifest);
    } catch (const std::exception &) {
        return false;
    }
    QProcess starter;
    starter.setProgram(program);
    starter.setArguments({"--run-job", job});
    starter.setWorkingDirectory(job);
    starter.setStandardInputFile(QProcess::nullDevice());
    starter.setStandardOutputFile(QProcess::nullDevice());
    starter.setStandardErrorFile(QProcess::nullDevice());
    qint64 started = 0;
    if (!starter.startDetached(&started))
        return false;
    if (pid)
        *pid = started;
    return true;
}
} // namespace mf
