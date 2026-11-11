// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/remote_execution.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/run_diagnostics.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <functional>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace mf {
namespace {
const QStringList schedulers{"shell", "slurm", "pbs"};
// Files of a job folder that stay on this computer: the record, the project, the user's note and what
// asks the supervisor to stop. The output and errors come back from the destination.
const QStringList localFiles{"run.json",        "project.molforma.json",
                             "memo.txt",        "stop.request",
                             "supervisor.lock", "output.out",
                             "stderr.err",      ".retrieved"};
// What the copy that comes back does not replace: the record and inputs of this computer.
const QStringList keptFiles{
    "run.json", "project.molforma.json", "memo.txt", "stop.request", "supervisor.lock", "input.nw", "job.sh"};
QString quote(QString s) {
    s.replace('\'', "'\\''");
    return "'" + s + "'";
}
bool controls(const QString &s, bool lines) {
    return s.contains(QRegularExpression(lines ? "[\\x00-\\x08\\x0b-\\x1f\\x7f]" : "[\\x00-\\x1f\\x7f]"));
}
// The folder of the jobs as the shell of that computer reads it: "~" is its home.
QString remoteRoot(const QString &directory) {
    if (directory == "~")
        return "\"$HOME\"";
    if (directory.startsWith("~/"))
        return "\"$HOME\"/" + quote(directory.mid(2));
    return quote(directory);
}
QString newNonce() {
    return QString("MOLFORMA-%1").arg(QRandomGenerator::global()->generate64(), 16, 16, QChar('0'));
}
// What a script printed between its markers: anything a login profile printed before is no part of it.
std::optional<QByteArray> answer(const QByteArray &out, const QString &nonce) {
    const auto begin = (nonce + "-BEGIN\n").toLatin1(), end = ("\n" + nonce + "-END").toLatin1();
    const auto from = out.indexOf(begin);
    const auto to = out.lastIndexOf(end);
    if (from < 0 || to < from + begin.size() - 1)
        return std::nullopt;
    return out.mid(from + begin.size(), to - from - begin.size());
}
QString scriptStart(const QString &nonce) {
    return "printf '%s\\n' " + quote(nonce + "-BEGIN") + "\n";
}
QString scriptEnd(const QString &nonce) {
    return "printf '\\n%s\\n' " + quote(nonce + "-END") + "\n";
}
struct Reply {
    int code = -1;
    QByteArray out, err;
    QString failure; // The program did not start, took too long or was cancelled.
    bool cancelled = false;
    // What ssh or the other side said: its last lines.
    QString detail() const {
        if (!failure.isEmpty())
            return failure;
        auto lines = QString::fromLocal8Bit(err).split('\n', Qt::SkipEmptyParts);
        for (auto &line : lines)
            line = line.trimmed();
        lines.removeAll(QString());
        if (lines.size() > 3)
            lines = lines.mid(lines.size() - 3);
        const auto text = lines.join(" / ");
        return text.isEmpty() ? QString("exit code %1").arg(code) : text.left(600);
    }
};
Reply execute(const QString &program, const QStringList &arguments, const QByteArray &input, qint64 timeout,
              const std::function<bool()> &cancelled) {
    Reply r;
    QProcess p;
    p.setProgram(program);
    p.setArguments(arguments);
    p.start();
    if (!p.waitForStarted(15000)) {
        r.failure = p.errorString();
        return r;
    }
    p.write(input);
    p.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    while (p.state() != QProcess::NotRunning && !p.waitForFinished(250)) {
        r.out += p.readAllStandardOutput();
        r.err += p.readAllStandardError();
        const bool stop = cancelled && cancelled();
        if (stop || (timeout > 0 && clock.elapsed() > timeout)) {
            p.kill();
            p.waitForFinished(5000);
            r.cancelled = stop;
            r.failure = stop ? QString("cancelled") : QString("no answer in %1 s").arg(timeout / 1000);
            return r;
        }
    }
    r.out += p.readAllStandardOutput();
    r.err += p.readAllStandardError();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    if (p.exitStatus() != QProcess::NormalExit)
        r.failure = p.errorString();
    return r;
}
QStringList sshOptions() {
    // Never a password prompt or a question about an unknown host key: what ssh cannot do by itself
    // fails, and the user sets it up once in a terminal (ssh-agent, known_hosts).
    return {"-o", "BatchMode=yes",          "-o", "ConnectTimeout=20",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"};
}
QStringList sshArguments(const RemoteHost &host) {
    auto arguments = sshOptions();
    if (host.port)
        arguments << "-p" << QString::number(host.port);
    // A login shell, which sets PATH and module as a terminal there would; the script comes on stdin, so
    // no shell of that computer reads it twice.
    return arguments << "--" << host.host << "bash -l -s";
}
// The connection to a destination, through the ssh and scp of this computer.
struct Connection {
    RemoteHost host;
    QString ssh = remoteProgram("ssh"), scp = remoteProgram("scp");
    Reply run(const QString &script, qint64 timeout = 180000) const {
        return execute(ssh, sshArguments(host), script.toUtf8(), timeout, {});
    }
    Reply copy(const QStringList &sources, const QString &target,
               const std::function<bool()> &cancelled) const {
        auto arguments = sshOptions();
        // Without -q, which would hide why ssh failed; scp shows no progress without a terminal.
        arguments << "-r" << "-p";
        if (host.port)
            arguments << "-P" << QString::number(host.port);
        return execute(scp, arguments << "--" << sources << target, {}, 0, cancelled);
    }
    QString remote(const QString &path) const {
        return host.host + ':' + path;
    }
};
const QRegularExpression &remotePath() {
    static const QRegularExpression pattern("^/[A-Za-z0-9._/+-]*$");
    return pattern;
}
const QRegularExpression &jobId() {
    static const QRegularExpression pattern("^[A-Za-z0-9._\\[\\]-]{1,128}$");
    return pattern;
}
QString stateName(const QString &state) {
    if (state == "queued")
        return tr("待機中", "remote");
    if (state == "running")
        return tr("実行中");
    if (state == "held")
        return tr("保留中", "remote");
    if (state == "ending")
        return tr("終了処理中", "remote");
    if (state == "ended")
        return tr("終了", "state");
    return tr("状態不明", "remote");
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
    RemoteSupervisor(QString job, QJsonObject record)
        : directory(std::move(job)), manifest(std::move(record)) {
        connection.host = RemoteHost::fromJson(remote()["host"].toObject());
    }
    int exec();

  private:
    QString directory;
    QJsonObject manifest;
    Connection connection;
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
        return QFileInfo::exists(path("stop.request"));
    }
    qint64 elapsed() const {
        const auto started = QDateTime::fromString(manifest["started_at"].toString(), Qt::ISODateWithMs);
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
    QSaveFile file(path("run.json"));
    const auto bytes = QJsonDocument(manifest).toJson();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        unwritable = 0;
    else
        ++unwritable;
}
int RemoteSupervisor::end(RunState state, const char *message, const QString &detail,
                          std::optional<int> exitCode) {
    status = state;
    // Kept in Japanese whatever the language: the run panel and the history show it through translated().
    manifest["error"] = message ? QString::fromUtf8(message) : QString();
    set("detail", detail.isEmpty() ? QJsonValue() : QJsonValue(detail));
    set("message", {});
    set("failing_since", {});
    set("phase", "done");
    if (exitCode) {
        manifest["exit_code"] = *exitCode;
        manifest["exit_status"] = "normal";
    } else {
        manifest["exit_code"] = -1;
        manifest["exit_status"] = "crash";
    }
    manifest["finished_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const auto *name : {"output.out", "stderr.err"})
        if (!QFileInfo(path(name)).isFile())
            writeFile(path(name), {});
    try {
        manifest["summary"] = nwchemLogSummary(path("output.out"));
    } catch (const std::exception &) {
        manifest.remove("summary");
    }
    QString output, errors;
    try {
        output = historyText(directory, "output.out", true);
    } catch (const std::exception &) {
    }
    try {
        errors = historyText(directory, "stderr.err", true);
    } catch (const std::exception &) {
    }
    // A job that never ran failed to start; the rest is for the log to tell.
    const bool started = remote().contains("job");
    auto diagnostics = diagnoseRun(state, output, errors,
                                   !started && message ? translated(QString::fromUtf8(message)) : QString());
    // A run that did not start there points at its destination, not at the programs of this PC.
    for (qsizetype i = 0; i < diagnostics.size(); ++i)
        if (auto item = diagnostics[i].toObject(); item["setting"].toString() == "executable") {
            item["setting"] = "remote";
            diagnostics[i] = item;
        }
    manifest["diagnostics"] = diagnostics;
    save();
    QFile::remove(path("stop.request"));
    return 0;
}
bool RemoteSupervisor::upload() {
    status = RunState::Starting;
    set("phase", "upload");
    save();
    for (const auto *name : {"output.out", "stderr.err"})
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
                            : std::max<qint64>(0, QFileInfo(path("output.out")).size());
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
    const auto registered = findRemoteHost(connection.host.name);
    if (!registered || registered->host != connection.host.host || registered->port != connection.host.port)
        return end(RunState::Failed,
                   QT_TR_NOOP("実行先が環境設定にないか、接続先が変わっています。「3 "
                              "実行設定」で実行先を確認してください。"),
                   connection.host.name);
    if (connection.ssh.isEmpty() || connection.scp.isEmpty())
        return end(RunState::Failed, QT_TR_NOOP("このPCに ssh と scp が見つかりません。"));
    manifest["supervisor"] = executionProcessIdentity(QCoreApplication::applicationPid());
    status = runStateFromKey(manifest["status"].toString());
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
                    QFile::remove(path("stop.request"));
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
                                        : QFileInfo(path("output.out")).size();
                if (!last.log.isEmpty() || last.from > offset) {
                    QFile log(path("output.out"));
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

QJsonObject RemoteHost::json() const {
    return {{"name", name},           {"host", host},           {"port", port},
            {"directory", directory}, {"scheduler", scheduler}, {"executable", executable},
            {"launcher", launcher},   {"header", header},       {"prelude", prelude},
            {"interval", interval}};
}
RemoteHost RemoteHost::fromJson(const QJsonObject &o) {
    RemoteHost h;
    h.name = o["name"].toString();
    h.host = o["host"].toString();
    h.port = o["port"].toInt();
    h.directory = o["directory"].toString(h.directory);
    h.scheduler = o["scheduler"].toString(h.scheduler);
    h.executable = o["executable"].toString();
    h.launcher = o["launcher"].toString();
    h.header = o["header"].toString();
    h.prelude = o["prelude"].toString();
    h.interval = o["interval"].toInt(h.interval);
    return h;
}
QStringList remoteSchedulers() {
    return schedulers;
}
QString remoteSchedulerName(const QString &scheduler) {
    if (scheduler == "slurm")
        return tr("Slurm（sbatch）");
    if (scheduler == "pbs")
        return tr("PBS（qsub）");
    return tr("シェル（バックグラウンド）");
}
QVector<RemoteHost> remoteHosts() {
    const auto stored =
        QJsonDocument::fromJson(QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                                    .value("execution/remotes")
                                    .toByteArray())
            .array();
    QVector<RemoteHost> hosts;
    for (const auto &value : stored)
        if (value.isObject())
            hosts << RemoteHost::fromJson(value.toObject());
    return hosts;
}
QStringList remoteHostErrors(const RemoteHost &h) {
    QStringList errors;
    const auto name = h.name.trimmed();
    if (name.isEmpty() || name.size() > 64 || controls(h.name, false) || name != h.name)
        errors << tr("実行先の名前は、前後に空白のない64文字以内で指定してください。");
    if (!QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9._@%+-]{0,254}$").match(h.host).hasMatch())
        errors << tr("接続先は「ユーザー名@ホスト名」か、~/.ssh/config "
                     "のホスト名で指定してください（英数字と . _ @ % + -）。");
    if (h.port < 0 || h.port > 65535)
        errors << tr("ポートは0（sshの設定どおり）か1〜65535で指定してください。");
    const auto parts = h.directory.split('/');
    if (h.directory.size() > 1024 ||
        !QRegularExpression("^(~|~/[A-Za-z0-9._/+-]*|/[A-Za-z0-9._/+-]*|[A-Za-z0-9._+][A-Za-z0-9._/+-]*)$")
             .match(h.directory)
             .hasMatch() ||
        parts.contains(".."))
        errors << tr(
            "実行先の作業フォルダーは、英数字と . _ / + - だけのパスで指定してください（~/ で始めるとホーム"
            "から、.. は使えません）。");
    if (!schedulers.contains(h.scheduler))
        errors << tr("投入方式は、シェル・Slurm・PBSから選んでください。");
    for (const auto &value : {h.executable, h.launcher})
        if (value.size() > 1024 || controls(value, false))
            errors << tr("実行先のNWChem・MPIランチャーに制御文字または長すぎる文字列があります。");
    if (!h.header.trimmed().isEmpty())
        errors << jobHeaderErrors(h.header);
    if (h.prelude.size() > 4000 || controls(h.prelude, true))
        errors << tr("準備の行は、制御文字を含めず4000文字以内で指定してください。");
    if (h.interval < 10 || h.interval > 3600)
        errors << tr("状態を確認する間隔は10〜3600秒で指定してください。");
    errors.removeDuplicates();
    return errors;
}
void saveRemoteHosts(const QVector<RemoteHost> &hosts) {
    QJsonArray stored;
    QSet<QString> names;
    for (const auto &h : hosts) {
        const auto errors = remoteHostErrors(h);
        if (!errors.isEmpty())
            fail(tr("実行先「%1」: %2").arg(h.name, errors.join('\n')));
        if (names.contains(h.name))
            fail(tr("実行先の名前が重複しています: %1").arg(h.name));
        names.insert(h.name);
        stored.append(h.json());
    }
    QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
        .setValue("execution/remotes", QJsonDocument(stored).toJson(QJsonDocument::Compact));
}
std::optional<RemoteHost> findRemoteHost(const QString &name) {
    if (name.isEmpty())
        return std::nullopt;
    for (const auto &h : remoteHosts())
        if (h.name == name)
            return h;
    return std::nullopt;
}
QString remoteHostSummary(const RemoteHost &h) {
    const auto where = h.port ? tr("%1（ポート %2）").arg(h.host).arg(h.port) : h.host;
    const auto kind = h.scheduler == "slurm" ? QString("Slurm")
                      : h.scheduler == "pbs" ? QString("PBS")
                                             : tr("シェル");
    return tr("%1:%2（%3）").arg(where, h.directory, kind);
}
QString remoteProgram(const QString &name) {
    return QStandardPaths::findExecutable(name);
}

QStringList jobHeaderPlaceholders() {
    return {"name", "ranks", "threads", "omp_threads", "blas_threads", "cpus", "time", "queue", "memory_mb"};
}
QString defaultJobHeader(const QString &scheduler) {
    if (scheduler == "slurm")
        return "#SBATCH --job-name={name}\n"
               "#SBATCH --nodes=1\n"
               "#SBATCH --ntasks={ranks}\n"
               "#SBATCH --cpus-per-task={threads}\n"
               "#SBATCH --time={time}\n"
               "#SBATCH --partition={queue}\n"
               "#SBATCH --output=scheduler-%j.log\n";
    if (scheduler == "pbs")
        return "#PBS -N {name}\n"
               "#PBS -l select=1:ncpus={cpus}:mpiprocs={ranks}:ompthreads={omp_threads}\n"
               "#PBS -l walltime={time}\n"
               "#PBS -q {queue}\n"
               "#PBS -j oe\n";
    return {};
}
QString jobHeaderTemplate(const QString &scheduler) {
    const auto stored = QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                            .value("execution/job_header_" + scheduler)
                            .toString();
    return stored.trimmed().isEmpty() || !jobHeaderErrors(stored).isEmpty() ? defaultJobHeader(scheduler)
                                                                            : stored;
}
void saveJobHeaderTemplate(const QString &scheduler, const QString &text) {
    if (scheduler != "slurm" && scheduler != "pbs")
        fail("Unknown scheduler: " + scheduler);
    if (const auto errors = jobHeaderErrors(text); !errors.isEmpty())
        fail(errors.join('\n'));
    QSettings settings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma");
    const auto key = "execution/job_header_" + scheduler;
    QStringList lines;
    for (const auto &line : text.split('\n'))
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    const auto normalized = lines.isEmpty() ? QString() : lines.join('\n') + '\n';
    if (normalized.isEmpty() || normalized == defaultJobHeader(scheduler))
        settings.remove(key);
    else
        settings.setValue(key, normalized);
}
QStringList jobHeaderErrors(const QString &text) {
    QStringList errors;
    if (text.size() > 4000)
        errors << tr("スクリプトの見出しは4000文字以内で指定してください。");
    if (controls(text, true))
        errors << tr("スクリプトの見出しに制御文字があります。");
    static const QRegularExpression placeholder("\\{([^{}]*)\\}");
    const auto known = jobHeaderPlaceholders();
    for (const auto &line : text.split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (!trimmed.startsWith('#'))
            errors << tr("スクリプトの見出しの行は # で始めてください: %1").arg(trimmed.left(80));
        for (auto it = placeholder.globalMatch(trimmed); it.hasNext();)
            if (const auto name = it.next().captured(1); !known.contains(name))
                errors << tr("見出しに不明なプレースホルダーがあります: {%1}").arg(name.left(40));
    }
    errors.removeDuplicates();
    return errors;
}
QStringList renderJobHeader(const QString &text, const QMap<QString, QString> &values) {
    static const QRegularExpression placeholder("\\{([a-z_]+)\\}");
    QStringList lines;
    for (const auto &line : text.split('\n')) {
        auto rendered = line.trimmed();
        if (rendered.isEmpty())
            continue;
        bool complete = true;
        for (auto it = placeholder.globalMatch(rendered); it.hasNext();)
            complete &= !values.value(it.next().captured(1)).isEmpty();
        if (!complete)
            continue;
        for (auto it = values.begin(); it != values.end(); ++it)
            rendered.replace('{' + it.key() + '}', it.value());
        lines << rendered;
    }
    return lines;
}

bool remoteRun(const QJsonObject &manifest) {
    return manifest["remote"].isObject() && !manifest["remote"].toObject().isEmpty();
}
QString remoteRunText(const QJsonObject &manifest) {
    if (!remoteRun(manifest))
        return {};
    const auto r = manifest["remote"].toObject();
    const auto host = RemoteHost::fromJson(r["host"].toObject());
    QStringList parts{tr("実行先 %1（%2）").arg(host.name, host.host)};
    const auto job = r["job"].toString();
    const auto phase = r["phase"].toString();
    if (phase.isEmpty() || phase == "prepared" || phase == "upload")
        parts << tr("ジョブを複写中");
    else if (phase == "submit")
        parts << tr("投入中");
    else {
        const auto what = host.scheduler == "shell" ? tr("プロセス %1").arg(job) : tr("ジョブ %1").arg(job);
        if (phase == "retrieve")
            parts << tr("%1 · 結果を回収中").arg(what);
        else if (phase == "done")
            parts << what;
        else {
            const auto raw = r["scheduler_state"].toString().section(' ', 0, 0);
            const auto state = stateName(r["state"].toString());
            parts << (raw.isEmpty() || host.scheduler != "slurm" ? tr("%1 · %2").arg(what, state)
                                                                 : tr("%1 · %2（%3）").arg(what, state, raw));
        }
    }
    if (const auto message = r["message"].toString(); !message.isEmpty()) {
        const auto since = QDateTime::fromString(r["failing_since"].toString(), Qt::ISODateWithMs);
        parts
            << (since.isValid()
                    ? tr("%1（%2から）").arg(translated(message), since.toLocalTime().toString("MM/dd HH:mm"))
                    : translated(message));
        if (const auto detail = r["detail"].toString(); !detail.isEmpty())
            parts << detail;
    } else if (const auto detail = r["detail"].toString(); !detail.isEmpty() && phase == "done")
        parts << detail;
    return parts.join(" · ");
}
int remoteJobMain(const QString &directory, QJsonObject manifest) {
    const auto state = runStateFromKey(manifest["status"].toString());
    const auto phase = manifest["remote"].toObject()["phase"].toString();
    QByteArray input;
    try {
        input = readFile(QDir(directory).filePath("input.nw"));
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
    QLockFile lock(QDir(directory).filePath("supervisor.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 2;
#ifdef Q_OS_UNIX
    // Logging out or shutting down ends only this supervisor: the job goes on there, and the next
    // supervisor takes it up. Only a stop request (stop.request) cancels it.
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        std::signal(signal, remoteSignal);
#endif
    RemoteSupervisor supervisor(directory, std::move(manifest));
    return supervisor.exec();
}
bool resumeRemoteRun(const QString &directory, const QString &program, qint64 *pid) {
    const auto job = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    QJsonObject manifest;
    try {
        manifest = parseJson(readFile(QDir(job).filePath("run.json")));
    } catch (const std::exception &) {
        return false;
    }
    if (manifest["format"] != "molforma-nwchem-run" || !remoteRun(manifest) ||
        !runStateActive(runStateFromKey(manifest["status"].toString())))
        return false;
    const auto keeper = manifest["supervisor"].toObject();
    if (!keeper.isEmpty() &&
        (keeper["host"].toString() != QSysInfo::machineHostName() || !executionProcessGone(keeper)))
        return false;
    // Until the new supervisor writes its own identity, the record names none that has ended.
    manifest.remove("supervisor");
    QSaveFile file(QDir(job).filePath("run.json"));
    const auto bytes = QJsonDocument(manifest).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return false;
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
QString remoteJobPreview(const Document &d) {
    const auto name = executionSettings(d)["remote"].toString();
    const auto host = findRemoteHost(name);
    if (!host)
        fail(tr("実行先「%1」はこのPCの環境設定に登録されていません。").arg(name));
    const QString submit = host->scheduler == "slurm" ? "sbatch job.sh"
                           : host->scheduler == "pbs" ? "qsub job.sh"
                                                      : "nohup ./job.sh &";
    const auto where = host->host + ':' + host->directory + '/' + nwchemSettings(d)["start"].toString() +
                       "-YYYYMMDD-HHMMSS-ID/";
    return tr("# ssh・scpで %1 "
              "へ複写し、そこで「%2」として投入します。終わると同じジョブフォルダーへ回収します。")
               .arg(where, submit) +
           '\n' + executionJobScript(d, &*host);
}

RemoteProbe::RemoteProbe(QObject *parent) : QObject(parent) {
    connect(&process, &QProcess::finished, this, &RemoteProbe::done);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !cancelling)
            emit finished(false, tr("ssh を起動できません: %1").arg(process.errorString()));
    });
}
RemoteProbe::~RemoteProbe() {
    cancel();
}
void RemoteProbe::start(const RemoteHost &host, const QString &program, const QString &mpi) {
    cancel();
    probed = host;
    executable = host.executable.isEmpty() ? program : host.executable;
    launcher = host.launcher.isEmpty() ? mpi : host.launcher;
    const auto ssh = remoteProgram("ssh");
    if (ssh.isEmpty()) {
        QTimer::singleShot(0, this, [this] { emit finished(false, tr("このPCに ssh が見つかりません。")); });
        return;
    }
    nonce = newNonce();
    QString script = scriptStart(nonce);
    // As the job script prepares NWChem: the lines of Preferences, then those of the destination.
    if (const auto prelude = executionJobPrelude(); !prelude.isEmpty())
        script += prelude + '\n';
    if (!host.prelude.trimmed().isEmpty())
        script += host.prelude + '\n';
    script += "printf 'host %s\\n' \"$(uname -n)\"\n"
              "for c in sbatch squeue scancel sacct qsub qstat qdel setsid; do\n"
              "  if command -v \"$c\" >/dev/null 2>&1; then printf 'has %s\\n' \"$c\"; fi\n"
              "done\n";
    script += "printf 'nwchem %s\\n' \"$(command -v " + quote(executable) + " 2>/dev/null)\"\n";
    if (!launcher.isEmpty())
        script += "printf 'launcher %s\\n' \"$(command -v " + quote(launcher) + " 2>/dev/null)\"\n";
    script +=
        "root=" + remoteRoot(host.directory) +
        "\nif mkdir -p -- \"$root\" 2>/dev/null && cd -- \"$root\" 2>/dev/null; then printf 'root %s\\n' "
        "\"$(pwd -P)\"; fi\n" +
        scriptEnd(nonce);
    process.setProgram(ssh);
    process.setArguments(sshArguments(host));
    process.start();
    process.write(script.toUtf8());
    process.closeWriteChannel();
    // A connection that hangs ends after a while (a later probe has a time of its own).
    QTimer::singleShot(90000, this, [this, probe = ++probes] {
        if (probe == probes && active())
            process.kill();
    });
}
void RemoteProbe::cancel() {
    if (!active())
        return;
    cancelling = true;
    process.kill();
    process.waitForFinished(3000);
    cancelling = false;
}
void RemoteProbe::done() {
    if (cancelling)
        return;
    const auto out = process.readAllStandardOutput();
    const auto err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    const auto text = answer(out, nonce);
    if (!text) {
        QStringList report{tr("接続できません。")};
        const auto lines = err.split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            report << lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join('\n');
        if (err.contains("Host key verification failed") || err.contains("No ED25519 host key") ||
            err.contains("host key"))
            report << tr("ホスト鍵が登録されていません。端末で一度「ssh "
                         "%1」を実行し、表示された鍵を確かめて登録してください。")
                          .arg(probed.host);
        else if (err.contains("Permission denied"))
            report << tr("認証できません。鍵を ssh-agent に登録するか、~/.ssh/config "
                         "で鍵を指定してください（パスワード"
                         "の入力には対応していません）。");
        emit finished(false, report.join('\n'));
        return;
    }
    QString host, root, nwchem, mpi;
    QStringList has;
    for (const auto &raw : QString::fromUtf8(*text).split('\n', Qt::SkipEmptyParts)) {
        const auto word = raw.section(' ', 0, 0), rest = raw.section(' ', 1).trimmed();
        if (word == "host")
            host = rest;
        else if (word == "root")
            root = rest;
        else if (word == "has")
            has << rest;
        else if (word == "nwchem")
            nwchem = rest;
        else if (word == "launcher")
            mpi = rest;
    }
    bool ok = true;
    QStringList report{tr("接続できました（%1）。").arg(host.isEmpty() ? probed.host : host)};
    if (root.isEmpty()) {
        ok = false;
        report << tr("作業フォルダー %1 を作れません。").arg(probed.directory);
    } else if (!remotePath().match(root).hasMatch()) {
        ok = false;
        report << tr("作業フォルダーのパスに使えない文字があります: %1").arg(root);
    } else
        report << tr("作業フォルダー: %1").arg(root);
    const QStringList needed = probed.scheduler == "slurm" ? QStringList{"sbatch", "squeue", "scancel"}
                               : probed.scheduler == "pbs" ? QStringList{"qsub", "qstat", "qdel"}
                                                           : QStringList{};
    QStringList missing;
    for (const auto &command : needed)
        if (!has.contains(command))
            missing << command;
    if (!missing.isEmpty()) {
        ok = false;
        report << tr("見つからないコマンド: %1").arg(missing.join(", "));
    } else if (!needed.isEmpty())
        report << tr("スケジューラー: %1").arg(needed.join(", "));
    if (probed.scheduler == "shell" && !has.contains("setsid"))
        report << tr("setsid がありません。停止はジョブのスクリプトとその子プロセスにだけ送ります。");
    report << (nwchem.isEmpty()
                   ? tr("確認: NWChem「%1」がログインしたときのPATHにありません（計算ノードでだけ使える場合"
                        "は問題ありません）。")
                         .arg(executable)
                   : tr("NWChem: %1").arg(nwchem));
    if (!launcher.isEmpty())
        report << (mpi.isEmpty()
                       ? tr("確認: MPIランチャー「%1」がログインしたときのPATHにありません。").arg(launcher)
                       : tr("MPIランチャー: %1").arg(mpi));
    emit finished(ok, report.join('\n'));
}
} // namespace mf
