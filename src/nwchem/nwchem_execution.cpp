// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_execution.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/remote_execution.h"
#include "nwchem/run_record.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace mf {
QString absolutePath(QString path, const QString &base) {
    if (path == "~")
        path = QDir::homePath();
    else if (path.startsWith("~/"))
        path = QDir::home().filePath(path.mid(2));
    return QDir::cleanPath(QDir(base).absoluteFilePath(path));
}
bool networkPath(const QString &path) {
    const auto p = path.trimmed();
    return p.startsWith("//") || p.startsWith("\\\\");
}
QString untrustedNetworkPath(const Document &d, const ExecutionEnvironment &environment) {
    const auto s = executionSettings(d);
    const auto &preferred = environment.preferences;
    for (const auto *key : {"executable", "launcher", "job_root", "reference_directory"})
        if (const auto value = s[key].toString(); networkPath(value) && value != preferred[key].toString())
            return value;
    for (const auto &row : s["environment"].toArray())
        if (const auto value = row.toObject()["value"].toString();
            networkPath(value) && !preferred["environment"].toArray().contains(row))
            return value;
    const auto advanced = nwchemAdvanced(d);
    for (const auto *key : {"vectors_input", "vectors_output", "scratch_dir", "permanent_dir"})
        if (const auto value = advanced[key].toString(); networkPath(value))
            return value;
    if (const auto job = nwchemRestart(nwchemSettings(d)).job;
        networkPath(job) && !networkPath(preferred["job_root"].toString()))
        return job;
    return {};
}
namespace {
const QStringList managedEnvironment{"OMP_NUM_THREADS",      "OMP_STACKSIZE",    "OMP_DYNAMIC",
                                     "OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS",
                                     "MKL_DYNAMIC",          "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS"};
const QStringList bundleFiles{JobFile::input,  JobFile::project, JobFile::record,  JobFile::output,
                              JobFile::errors, JobFile::script,  JobFile::exitCode};
bool controls(const QString &s) {
    return s.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
QString locateProgram(const QString &name, const QString &base, const QProcessEnvironment &environment) {
    if (QDir::isAbsolutePath(name) || name.contains('/') || name.contains(QDir::separator()) ||
        name.startsWith("~"))
        return absolutePath(name, base);
    const auto paths = environment.value("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    return paths.isEmpty() ? QString() : QStandardPaths::findExecutable(name, paths);
}
bool writableDirectory(const QString &path, bool mayCreate) {
    QFileInfo info(path);
    if (mayCreate)
        while (!info.exists() && info.absoluteFilePath() != info.absolutePath())
            info.setFile(info.absolutePath());
    return info.isDir() && info.isWritable();
}
QString quote(QString s) {
    s.replace('\'', "'\\''");
    return "'" + s + "'";
}
// How job.sh and the command shown for a run send NWChem's output and errors to their files.
QString outputRedirection() {
    return QString(" > %1 2> %2").arg(QString(JobFile::output), QString(JobFile::errors));
}
#ifdef Q_OS_WIN
// Defined last in this file, away from the macros of windows.h.
bool windowsProcessRunning(qint64 pid);
void windowsTerminateTree(qint64 pid);
#endif
QString bootId() {
    QFile f("/proc/sys/kernel/random/boot_id");
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(64)).trimmed() : QString();
}
// Start time of a process in clock ticks after boot (field 22 of /proc/<pid>/stat), or empty.
QString processStart(qint64 pid) {
    QFile f(QString("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const auto stat = QString::fromLatin1(f.read(4096));
    return stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(19);
}
QString executionRoot(const QJsonObject &s, const QString &base) {
    return absolutePath(s["job_root"].toString(), base);
}
// The file that asks the supervisor of a job to stop it.
QString stopRequest(const QString &directory) {
    return jobFile(directory, JobFile::stopRequest);
}
// Ends the process group of a run (Unix), or the whole tree of its first process (Windows).
void signalGroup(qint64 group, bool force) {
    if (group <= 0)
        return;
#ifdef Q_OS_UNIX
    ::kill(-pid_t(group), force ? SIGKILL : SIGTERM);
#elif defined(Q_OS_WIN)
    // No process groups and no signal a console program would heed: the forced stop ends the whole
    // tree (cmd, wsl.exe, mpiexec and NWChem), not only the program MolForma started.
    if (force)
        windowsTerminateTree(group);
#else
    Q_UNUSED(force);
#endif
}
#ifdef Q_OS_UNIX
volatile std::sig_atomic_t stopSignalled = 0;
void stopSignal(int) {
    stopSignalled = 1;
}
#endif
} // namespace
bool runSupervised(const QJsonObject &manifest) {
    const auto supervisor = manifest["supervisor"].toObject();
    return supervisor["pid"].toInteger() > 0 &&
           supervisor["host"].toString() == QSysInfo::machineHostName() && !executionProcessGone(supervisor);
}
QString runSupervisorProgram() {
    const auto image = qEnvironmentVariable("APPIMAGE");
    return QFileInfo(image).isFile() ? image : QCoreApplication::applicationFilePath();
}
bool executionRunGone(const QJsonObject &manifest) {
    const auto supervisor = manifest["supervisor"].toObject();
    if (supervisor.isEmpty())
        return executionProcessGone(manifest);
    return executionProcessGone(supervisor) && (!manifest.contains("pid") || executionProcessGone(manifest));
}
bool requestRunStop(const QString &directory) {
    QJsonObject manifest;
    try {
        manifest = readRunRecord(directory, RecordCheck::Json);
    } catch (const std::exception &) {
        return false;
    }
    if (!runStateActive(RunRecord(manifest).state()) || !runSupervised(manifest))
        return false;
    QFile request(stopRequest(directory));
    return request.open(QIODevice::WriteOnly);
}
int runJobMain(int argc, char **argv) {
    if (argc != 3 || QString::fromLocal8Bit(argv[1]) != "--run-job")
        return -1;
    QCoreApplication app(argc, argv);
    const auto directory = QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath());
    ExecutionPlan plan;
    try {
        // Only the job MolForma has just prepared and marked as starting, with the input it saved.
        const auto manifest = readRunRecord(directory, RecordCheck::Json);
        const RunRecord record(manifest);
        // A run on another computer: this process copies it there, follows it and fetches it back.
        if (record.hasFormat() && remoteRun(manifest))
            return remoteJobMain(directory, manifest);
        const auto input = readFile(jobFile(directory, JobFile::input));
        if (!record.hasFormat() || record.state() != RunState::Starting ||
            manifest["input_sha256"].toString() !=
                QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()))
            return 2;
        plan = runRecordPlan(directory, manifest);
        plan.manifest["supervisor"] = executionProcessIdentity(QCoreApplication::applicationPid());
    } catch (const std::exception &) {
        return 2;
    }
#ifdef Q_OS_UNIX
    for (const int signal : {SIGTERM, SIGINT, SIGHUP})
        std::signal(signal, stopSignal);
#endif
    NwchemRun run;
    QTimer requests;
    requests.setInterval(250);
    QObject::connect(&requests, &QTimer::timeout, &run, [&] {
        bool asked = QFileInfo::exists(stopRequest(directory));
#ifdef Q_OS_UNIX
        asked |= stopSignalled != 0;
#endif
        if (asked)
            run.stop();
    });
    QObject::connect(&run, &NwchemRun::changed, &app, [&] {
        if (!run.active())
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
    });
    run.startPrepared(plan);
    requests.start();
    const int code = run.active() ? app.exec() : 0;
    QFile::remove(stopRequest(directory));
    return code;
}
QJsonObject executionDefaults() {
    return {{"executable", "nwchem"},
            {"mode", "mpi"},
            {"launcher", "mpirun"},
            {"mpi_ranks", 2},
            {"omp_threads", 1},
            {"blas_threads", 1},
            {"omp_stacksize", ""},
            {"job_root", QDir::home().filePath("MolForma/jobs")},
            {"reference_directory", ""},
            {"timeout_seconds", 0},
            {"environment", QJsonArray{}},
            {"job_script", "none"},
            {"job_queue", ""},
            {"remote", ""}};
}
const QVector<ExecutionRange> &executionRanges() {
    static const QVector<ExecutionRange> ranges{{"mpi_ranks", 1, 4096},
                                                {"omp_threads", 1, 1024},
                                                {"blas_threads", 1, 1024},
                                                {"timeout_seconds", 0, 604800}};
    return ranges;
}
ExecutionRange executionRange(const QString &key) {
    for (const auto &range : executionRanges())
        if (range.key == key)
            return range;
    fail("Unknown execution setting: " + key);
}
QStringList executionPreferenceKeys() {
    return {"executable",   "mode",          "launcher", "mpi_ranks",       "omp_threads",
            "blas_threads", "omp_stacksize", "job_root", "timeout_seconds", "environment",
            "job_script",   "job_queue",     "remote"};
}
QJsonObject executionSettings(const Document &d) {
    auto result = executionDefaults();
    const auto stored = d.data["nwchem_execution"].toObject();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        result[it.key()] = it.value();
    return result;
}
void setExecutionSetting(Document &d, const QString &key, const QJsonValue &value) {
    auto s = d.data.value("nwchem_execution").toObject();
    if (value.isUndefined())
        s.remove(key);
    else
        s[key] = value;
    if (!s.isEmpty() || d.data.contains("nwchem_execution"))
        d.data["nwchem_execution"] = s;
}
ExecutionValidation validateExecution(const Document &d, const QString &base, bool local,
                                      const ExecutionEnvironment &environment) {
    ExecutionValidation r;
    const auto s = executionSettings(d);
    const auto calculation = validateNwchem(d);
    static_cast<ValidationReport &>(r) = calculation;
    if (d.data.contains("nwchem_execution") && !d.data["nwchem_execution"].isObject())
        r.error("execution.object", "nwchem_execution",
                tr("NWChem実行設定はオブジェクトである必要があります。"), {}, SettingTarget::Execution);
    const auto defaults = executionDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto v = s[it.key()];
        if (v.type() != it.value().type())
            r.error("execution.type", it.key(), tr("実行設定の型が不正です: %1").arg(it.key()), {},
                    SettingTarget::Execution);
        else if (v.isString() && (controls(v.toString()) || v.toString().size() > 4096))
            r.error("execution.string", it.key(),
                    tr("実行設定に制御文字または長すぎる文字列があります: %1").arg(it.key()), {},
                    SettingTarget::Execution);
    }
    auto integer = [&](const QString &key, int lo, int hi) {
        const double n = s[key].toDouble(-1);
        if (!s[key].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < lo || n > hi)
            r.error("execution.integer", key,
                    tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(lo).arg(hi), {},
                    SettingTarget::Execution);
    };
    const bool mpi = s["mode"] == "mpi";
    if (!mpi && s["mode"] != "direct")
        r.error("execution.mode", "mode", tr("起動方式はMPIまたは直接実行を選んでください。"), {},
                SettingTarget::Execution);
    for (const auto &range : executionRanges())
        integer(range.key, range.minimum, range.maximum);
    for (const auto &key : {"executable", "job_root"})
        if (s[key].toString().trimmed().isEmpty())
            r.error("execution.required", key, tr("%1 を指定してください。").arg(key), {},
                    SettingTarget::Execution);
    if (mpi && s["launcher"].toString().trimmed().isEmpty())
        r.error("execution.launcher", "launcher", tr("MPIランチャーを指定してください。"), {},
                SettingTarget::Execution);
    if (!QStringList{"none", "shell", "slurm", "pbs"}.contains(s["job_script"].toString()))
        r.error("execution.job_script", "job_script",
                tr("ジョブスクリプトは、作らない・シェル・Slurm・PBSから選んでください。"), {},
                SettingTarget::Execution);
    if (!QRegularExpression("^[A-Za-z0-9_.@:+-]{0,64}$").match(s["job_queue"].toString()).hasMatch())
        r.error("execution.job_queue", "job_queue",
                tr("キュー・パーティション名は英数字と _ . @ : + - の64文字以内で指定してください。"), {},
                SettingTarget::Execution);
    const auto stack = s["omp_stacksize"].toString();
    if (!stack.isEmpty() && !QRegularExpression("^[1-9][0-9]{0,8}[BKMGbkmg]?$").match(stack).hasMatch())
        r.error("execution.stack", "omp_stacksize",
                tr("OpenMPスタックは64M・1G等の正の整数と単位B/K/M/Gで指定してください。"), {},
                SettingTarget::Execution);
    r.environment = {{"OMP_NUM_THREADS", QString::number(s["omp_threads"].toInt())},
                     {"OMP_DYNAMIC", "FALSE"},
                     {"MKL_DYNAMIC", "FALSE"}};
    for (const auto &key : {"OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS", "BLIS_NUM_THREADS",
                            "VECLIB_MAXIMUM_THREADS"})
        r.environment[key] = QString::number(s["blas_threads"].toInt());
    if (!stack.isEmpty())
        r.environment["OMP_STACKSIZE"] = stack.toUpper();
    QSet<QString> seen;
    for (const auto &entry : s["environment"].toArray()) {
        const auto row = entry.toObject();
        if (!entry.isObject() || !row["enabled"].isBool()) {
            r.error("execution.environment_enabled", "environment",
                    tr("環境変数の行に有効・無効の指定がありません。"), {}, SettingTarget::Execution);
            continue;
        }
        if (!row["enabled"].toBool())
            continue;
        const auto name = row["name"].toString(), value = row["value"].toString();
        if (!row["name"].isString() ||
            !QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(name).hasMatch() ||
            !row["value"].isString() || controls(value) || value.size() > 8192) {
            r.error("execution.environment_value", "environment",
                    tr("有効な環境変数の名前・値を確認してください。"), {}, SettingTarget::Execution);
            continue;
        }
        if (seen.contains(name) || managedEnvironment.contains(name)) {
            r.error("execution.environment_duplicate", "environment",
                    tr("環境変数が重複、または専用の資源設定と重複しています: %1").arg(name), {},
                    SettingTarget::Execution);
            continue;
        }
        seen.insert(name);
        r.environment[name] = value;
    }
    if (r.environment.contains("NWCHEM_BASIS_LIBRARY") && !r.environment["NWCHEM_BASIS_LIBRARY"].isEmpty())
        r.environment["NWCHEM_BASIS_LIBRARY"] =
            absolutePath(r.environment["NWCHEM_BASIS_LIBRARY"], base) + '/';
    auto variables = QProcessEnvironment::systemEnvironment();
    for (auto it = r.environment.begin(); it != r.environment.end(); ++it)
        variables.insert(it.key(), it.value());
    r.referenceDirectory = s["reference_directory"].toString().isEmpty()
                               ? absolutePath(".", base)
                               : absolutePath(s["reference_directory"].toString(), base);
    // A remote run starts NWChem on its destination: the programs are named for that computer.
    const auto remoteName = s["remote"].toString();
    const auto remote = environment.remote(remoteName);
    if (!remoteName.isEmpty()) {
        if (!remote)
            r.error("execution.remote", "remote",
                    tr("実行先「%1」はこのPCの環境設定に登録されていません。「3 "
                       "実行設定」で実行先を選び直してくだ"
                       "さい。")
                        .arg(remoteName),
                    {}, SettingTarget::Execution);
        else
            r.remote = remoteHostSummary(*remote);
        if (nwchemAdvanced(d)["vectors_guess"].toString() == "file")
            r.error("execution.remote_vectors", "remote",
                    tr("リモート実行では、ファイルから読む初期軌道（入力movecs）を使えません。前のジョブから"
                       "続けると"
                       "きは「履歴」の「続きから実行を準備」を使ってください。"),
                    {}, SettingTarget::Execution);
    } else {
        r.executable = locateProgram(s["executable"].toString(), base, variables);
        r.launcher = locateProgram(s["launcher"].toString(), base, variables);
    }
    if (const auto network = untrustedNetworkPath(d, environment); !network.isEmpty()) {
        r.error(
            "execution.network", "job_root",
            tr("ネットワーク上の場所 %1 は、「編集 → 環境設定」で指定した場合だけ使えます。").arg(network),
            {}, SettingTarget::Execution);
        local = false; // Not even looked at.
    }
    if (local) {
        const auto check = [&](const QString &key, const QString &label, bool ok, const QString &found,
                               const QString &error) {
            r.checks << ExecutionCheck{key, label, ok ? found : error, ok ? QString() : error, ok};
            if (!ok)
                r.error("execution.local", key, error, {}, SettingTarget::Execution);
        };
        const auto program = [&](const QString &key, const QString &path, const QString &label,
                                 const QString &error) {
            const QFileInfo f(path);
            check(key, label, !path.isEmpty() && f.isFile() && f.isExecutable(), path, error);
        };
        if (!remoteName.isEmpty()) {
            check("remote", tr("リモート実行先"), remote.has_value(), r.remote,
                  tr("実行先「%1」が環境設定にありません。").arg(remoteName));
            const auto ssh = remoteProgram("ssh"), scp = remoteProgram("scp");
            check("remote", tr("ssh・scp"), !ssh.isEmpty() && !scp.isEmpty(), ssh + ", " + scp,
                  tr("このPCに ssh と scp が見つかりません（OpenSSHのクライアントが必要です）。"));
        } else {
            program("executable", r.executable, tr("NWChem実行ファイル"),
                    tr("NWChem実行ファイルが見つからないか、実行権限がありません。"));
            if (mpi)
                program("launcher", r.launcher, tr("MPIランチャー"),
                        tr("MPIランチャーが見つからないか、実行権限がありません。"));
        }
        check("reference_directory", tr("参照ファイルの基準"), QFileInfo(r.referenceDirectory).isDir(),
              r.referenceDirectory, tr("参照ファイルの基準ディレクトリがありません。"));
        const auto root = executionRoot(s, base);
        check("job_root", tr("ジョブ保存先"), writableDirectory(root, true), root,
              tr("ジョブ保存先にディレクトリを作成できません。"));
        if (nwchemRestart(nwchemSettings(d)).active()) {
            const auto database = restartSource(d);
            const QFileInfo info(database);
            check("restart", tr("再開元のデータベース"),
                  !database.isEmpty() && info.isFile() && !info.isSymLink() && info.isReadable(), database,
                  tr("再開元のジョブフォルダーにNWChemのデータベース（計算名.db）がありません。計算名とper"
                     "manent_dirを再開元のままにするか、「2 計算条件」で再開を解除してください。"));
        }
        const auto basis = r.environment.value("NWCHEM_BASIS_LIBRARY");
        if (!basis.isEmpty() && remoteName.isEmpty())
            check("environment", "NWCHEM_BASIS_LIBRARY", QFileInfo(basis).isDir(), basis,
                  tr("NWCHEM_BASIS_LIBRARYのディレクトリがありません。"));
        // The input's own errors stay in the list; the check only counts them.
        r.checks << ExecutionCheck{{},
                                   tr("NWChem入力"),
                                   calculation.errors.isEmpty() ? tr("生成できます")
                                                                : tr("生成できません（エラー %n 件）",
                                                                     nullptr, int(calculation.errors.size())),
                                   {},
                                   calculation.errors.isEmpty()};
    }
    const int ranks = mpi ? s["mpi_ranks"].toInt() : 1;
    if (mpi && ranks == 1)
        r.warning("execution.mpi_pr", "mpi_ranks",
                  tr("MPI-PR版NWChemは補助プロセスを必要とします。1計算プロセスでもMPI起動数2が必要です。"),
                  {}, SettingTarget::Execution);
    if (!mpi)
        r.warning("execution.direct", "mode",
                  tr("直接実行は対応する逐次版等に使用してください。MPI版はMPIランチャーを選んでください。"),
                  {}, SettingTarget::Execution);
    if (remoteName.isEmpty() &&
        ranks * std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt()) > QThread::idealThreadCount())
        r.warning("execution.cpus", "mpi_ranks", tr("指定した並列度が、このPCの論理CPU数を超えています。"),
                  {}, SettingTarget::Execution);
    if (s["omp_threads"].toInt() > 1 && s["blas_threads"].toInt() > 1)
        r.warning("execution.threads", "omp_threads",
                  tr("OpenMPとBLASの多重並列により、想定より多くのスレッドを使う場合があります。"), {},
                  SettingTarget::Execution);
    for (auto it = s.begin(); it != s.end(); ++it)
        if (!defaults.contains(it.key()))
            r.warning("execution.unknown", it.key(), tr("未対応の実行設定を保持しています: %1").arg(it.key()),
                      {}, SettingTarget::Execution);
    r.errors.removeDuplicates();
    return r;
}
ExecutionPlan executionPlan(const Document &d, const QString &base, const QString &directory, bool local,
                            const ExecutionEnvironment &environment) {
    const auto report = validateExecution(d, base, local, environment);
    if (!report.errors.isEmpty())
        throw ValidationFailure(report);
    const auto s = executionSettings(d);
    ExecutionPlan p;
    p.directory = QDir::cleanPath(directory);
    p.timeoutSeconds = s["timeout_seconds"].toInt();
    p.environment = report.environment;
    // A remote run: the programs as job.sh starts them there (the destination's names come first), and
    // the folders of NWChem are those of that computer, not looked at here.
    const auto remote = environment.remote(s["remote"].toString());
    const bool here = local && !remote;
    QString executable = report.executable.isEmpty() ? s["executable"].toString() : report.executable;
    QString launcher = report.launcher.isEmpty() ? s["launcher"].toString() : report.launcher;
    if (remote) {
        executable = remote->executable.isEmpty() ? s["executable"].toString() : remote->executable;
        launcher = remote->launcher.isEmpty() ? s["launcher"].toString() : remote->launcher;
    }
    p.program = executable;
    if (s["mode"] == "mpi") {
        p.program = launcher;
        p.arguments = {"-np", QString::number(s["mpi_ranks"].toInt()), executable};
    }
    p.arguments << JobFile::input;
    Document resolved = d;
    const auto advanced = nwchemAdvanced(d);
    if (advanced["vectors_guess"] == "file") {
        const auto input = absolutePath(advanced["vectors_input"].toString(), report.referenceDirectory);
        if (local && (!QFileInfo(input).isFile() || !QFileInfo(input).isReadable()))
            fail(tr("入力movecsを読めません: %1").arg(input));
        setNwchemAdvanced(resolved, "vectors_input", input);
    }
    for (const auto &key : {"scratch_dir", "permanent_dir", "vectors_output"}) {
        if (!advanced.contains(key))
            continue;
        const auto value = advanced[key].toString();
        const bool absolute = QDir::isAbsolutePath(value);
        const auto clean = QDir::cleanPath(value);
        if (!absolute && (clean == ".." || clean.startsWith("../")))
            fail(tr("%1 の相対パスはジョブフォルダー内を指定してください。").arg(key));
        const auto actual = QString(key) == "vectors_output" ? nwchemPermanentFile(d, value) : value;
        const auto target = absolutePath(actual, directory);
        if (QString(key) == "vectors_output") {
            if (!absolute && bundleFiles.contains(clean))
                fail(tr("出力movecsにはジョブ保存用ファイルと異なる名前を指定してください。"));
            // A restart goes on writing the orbitals it copied from the job it continues.
            if (here && !nwchemRestart(nwchemSettings(d)).active() &&
                (QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
                fail(tr("出力movecsは既に存在します。別の名前を指定してください: %1").arg(target));
            if (here && QDir::isAbsolutePath(actual) &&
                !writableDirectory(QFileInfo(target).absolutePath(), false))
                fail(tr("出力movecsの保存先がないか、書込みできません: %1").arg(target));
        } else if (here && absolute) {
            if (!writableDirectory(target, false))
                fail(tr("指定されたNWChemディレクトリがないか、書込みできません: %1").arg(target));
            const auto prefix = nwchemSettings(d)["start"].toString();
            if (!QDir(target)
                     .entryList({prefix + ".*", prefix + "_opt*", prefix + "_vib*"},
                                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                     .isEmpty())
                fail(tr("同じ計算名のファイルが指定ディレクトリにあります。計算名か保存先を変更してください: "
                        "%1")
                         .arg(target));
        }
    }
    const auto outputs = nwchemOutputFiles(d, validateNwchem(d));
    for (const auto &file : outputs) {
        const auto target = absolutePath(file, directory);
        if (local && (QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
            fail(tr("可視化ファイルは既に存在します。別の計算名・出力名を指定してください: %1").arg(target));
        if (advanced["vectors_guess"] == "file" &&
            target == resolved.data["nwchem"].toObject()["advanced"].toObject()["vectors_input"].toString())
            fail(tr("可視化ファイルと入力movecsには別の保存先を指定してください。"));
    }
    const auto generated = generateNwchemInput(resolved);
    p.input = generated.text;
    p.inputOutline = generated.outline;
    QJsonObject overrides;
    for (auto it = p.environment.begin(); it != p.environment.end(); ++it)
        overrides[it.key()] = it.value();
    p.manifest = newRunRecord();
    const QJsonObject saved{
        {"status", runStateKey(RunState::Prepared)},
        {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"directory", directory},
        {"program", p.program},
        {"arguments", QJsonArray::fromStringList(p.arguments)},
        {"environment_overrides", overrides},
        {"reference_directory", report.referenceDirectory},
        {"project_directory", QFileInfo(base).absoluteFilePath()},
        {"calculation", nwchemSettings(d)},
        {"formula", d.formula()},
        {"timeout_seconds", p.timeoutSeconds},
        {"input_sha256",
         QString::fromLatin1(
             QCryptographicHash::hash(p.input.toUtf8(), QCryptographicHash::Sha256).toHex())}};
    for (auto it = saved.begin(); it != saved.end(); ++it)
        p.manifest[it.key()] = it.value();
    if (!outputs.isEmpty())
        p.manifest["visualization_files"] = QJsonArray::fromStringList(outputs);
    // The destination as the environment has it: the supervisor follows the job there even if Preferences
    // change.
    if (remote)
        p.manifest["remote"] = QJsonObject{{"host", remote->json()}, {"phase", "prepared"}};
    return p;
}
ExecutionPlan executionPreviewPlan(const Document &d, const QString &base, bool local,
                                   const ExecutionEnvironment &environment) {
    const auto root = executionRoot(executionSettings(d), base);
    const auto job = QDir(root).filePath(nwchemSettings(d)["start"].toString() + "-YYYYMMDD-HHMMSS-ID");
    return executionPlan(d, base, job, local, environment);
}
QString executionCommand(const ExecutionPlan &p) {
    if (remoteRun(p.manifest)) {
        const auto host = RemoteHost::fromJson(p.manifest["remote"].toObject()["host"].toObject());
        const auto job = host.directory + '/' + QFileInfo(p.directory).fileName();
        QString command = "scp -r -- " + quote(QFileInfo(p.directory).fileName()) + ' ' +
                          quote(host.host + ':' + host.directory + '/') + "\nssh " + quote(host.host) +
                          " 'cd " + job + " && ";
        command += host.scheduler == "slurm" ? QString("sbatch %1'").arg(QString(JobFile::script))
                   : host.scheduler == "pbs" ? QString("qsub %1'").arg(QString(JobFile::script))
                                             : QString("nohup ./%1 &'").arg(QString(JobFile::script));
        QStringList program{quote(p.program)};
        for (const auto &arg : p.arguments)
            program << quote(arg);
        return command + "\n# " + JobFile::script + ": " + program.join(' ') + outputRedirection();
    }
    QString command = "cd -- " + quote(p.directory) + "\nenv";
    for (auto it = p.environment.begin(); it != p.environment.end(); ++it)
        command += ' ' + quote(it.key() + '=' + it.value());
    command += ' ' + quote(p.program);
    for (const auto &arg : p.arguments)
        command += ' ' + quote(arg);
    return command + outputRedirection();
}
QString executionJobScript(const Document &d, const RemoteHost *remote,
                           const ExecutionEnvironment &environment) {
    const auto s = executionSettings(d);
    const auto kind = remote ? remote->scheduler : s["job_script"].toString();
    if (kind == "none")
        return {};
    const auto report = validateExecution(d, QDir::homePath(), false, environment);
    if (!report.errors.isEmpty())
        throw ValidationFailure(report);
    const bool mpi = s["mode"] == "mpi";
    const int ranks = mpi ? s["mpi_ranks"].toInt() : 1;
    const int threads = std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt());
    const int limit = s["timeout_seconds"].toInt();
    const auto time = QString("%1:%2:%3")
                          .arg(limit / 3600, 2, 10, QChar('0'))
                          .arg(limit / 60 % 60, 2, 10, QChar('0'))
                          .arg(limit % 60, 2, 10, QChar('0'));
    const auto name = nwchemSettings(d)["start"].toString();
    // The values of the placeholders of the header; a line whose value is empty is left out.
    const QMap<QString, QString> values{
        {"name", name},
        {"ranks", QString::number(ranks)},
        {"threads", QString::number(threads)},
        {"omp_threads", QString::number(s["omp_threads"].toInt())},
        {"blas_threads", QString::number(s["blas_threads"].toInt())},
        {"cpus", QString::number(ranks * threads)},
        {"time", limit ? time : QString()},
        {"queue", s["job_queue"].toString()},
        {"memory_mb", QString::number(nwchemSettings(d)["memory_mb"].toInt())}};
    QStringList lines{"#!/bin/bash"};
    if (kind == "slurm" || kind == "pbs") {
        lines << renderJobHeader(environment.jobHeader(kind), values);
        if (remote)
            lines << renderJobHeader(remote->header, values);
    }
    if (remote)
        lines << "# Generated by MolForma for the destination " + remote->name +
                     ". MolForma copies this folder there, starts this"
              << "# script, follows the job and copies the folder back when it ends.";
    else
        lines << "# Generated by MolForma. Runs NWChem in the folder of this script; copy the folder back and"
              << QString("# register %1 in MolForma's history afterwards.").arg(QString(JobFile::output));
    lines << (kind == "slurm" ? "cd \"${SLURM_SUBMIT_DIR:-$(dirname \"$0\")}\" || exit 1"
              : kind == "pbs" ? "cd \"${PBS_O_WORKDIR:-$(dirname \"$0\")}\" || exit 1"
                              : "cd \"$(dirname \"$0\")\" || exit 1");
    if (!environment.jobPrelude.isEmpty())
        lines << environment.jobPrelude;
    if (remote && !remote->prelude.trimmed().isEmpty())
        lines << remote->prelude.trimmed();
    for (auto it = report.environment.begin(); it != report.environment.end(); ++it) {
        // The resources are this job's; other variables name places of the computer they were set on.
        const auto line = "export " + it.key() + '=' + quote(it.value());
        lines << (managedEnvironment.contains(it.key()) ? line : "# " + line);
    }
    // The programs as they are named in the settings (or by the destination), not where this computer
    // found them.
    const auto executable =
        remote && !remote->executable.isEmpty() ? remote->executable : s["executable"].toString();
    const auto launcher = remote && !remote->launcher.isEmpty() ? remote->launcher : s["launcher"].toString();
    QStringList command;
    if (mpi)
        command << quote(launcher) << "-np" << QString::number(ranks);
    command << quote(executable) << JobFile::input;
    lines << command.join(' ') + outputRedirection();
    // How NWChem ended, for whoever looks at the folder afterwards (MolForma, when it fetches a remote run).
    lines << "status=$?" << QString("echo \"$status\" > %1").arg(QString(JobFile::exitCode))
          << "exit \"$status\"";
    return lines.join('\n') + '\n';
}
ExecutionPlan saveExecutionBundle(const Document &d, const QString &base, bool forRun,
                                  const ExecutionEnvironment &environment) {
    const auto report = validateExecution(d, base, forRun, environment);
    if (!report.errors.isEmpty())
        throw ValidationFailure(report);
    const auto root = executionRoot(executionSettings(d), base);
    if (!QDir().mkpath(root))
        fail(tr("ジョブ保存先を作成できません: %1").arg(root));
    // The folder name shows the local time the user sees in the history; the records stay in UTC.
    QTemporaryDir directory(QDir(root).filePath(nwchemSettings(d)["start"].toString() + '-' +
                                                QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") +
                                                "-XXXXXX"));
    if (!directory.isValid())
        fail(tr("ジョブフォルダーを作成できません: %1").arg(directory.errorString()));
    auto plan = executionPlan(d, base, directory.path(), forRun, environment);
    const auto advanced = nwchemAdvanced(d);
    for (const auto &key : {"scratch_dir", "permanent_dir", "vectors_output"}) {
        if (!advanced.contains(key))
            continue;
        QString path = advanced[key].toString();
        if (QString(key) == "vectors_output")
            path = nwchemPermanentFile(d, path);
        if (QDir::isAbsolutePath(path))
            continue;
        if (QString(key) == "vectors_output")
            path = QFileInfo(path).path();
        if (!QDir(directory.path()).mkpath(path))
            fail(tr("ジョブ内の出力ディレクトリを作成できません: %1").arg(path));
    }
    writeFile(directory.filePath(JobFile::input), plan.input.toUtf8());
    const auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    const auto project = QJsonDocument(d.project()).toJson();
    writeFile(directory.filePath(JobFile::project), project);
    plan.manifest["project_sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(project, QCryptographicHash::Sha256).toHex());
    copyRestartFiles(d, directory.path());
    // A job saved to run elsewhere takes its start script along, and so does a run on a remote destination.
    const auto remote = environment.remote(executionSettings(d)["remote"].toString());
    const auto script = forRun ? (remote ? executionJobScript(d, &*remote, environment) : QString())
                               : executionJobScript(d, nullptr, environment);
    if (!script.isEmpty()) {
        const auto path = directory.filePath(JobFile::script);
        writeFile(path, script.toUtf8());
        QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeUser);
        plan.manifest["job_script"] = JobFile::script;
    }
    writeRunRecord(plan.directory, plan.manifest);
    directory.setAutoRemove(false);
    return plan;
}

QJsonObject executionProcessIdentity(qint64 pid) {
    QJsonObject identity{{"host", QSysInfo::machineHostName()}, {"pid", pid}};
    if (const auto boot = bootId(); !boot.isEmpty())
        identity["boot_id"] = boot;
    if (const auto start = processStart(pid); !start.isEmpty())
        identity["pid_start"] = start;
    return identity;
}
bool executionProcessGone(const QJsonObject &m) {
#ifdef Q_OS_UNIX
    if (m["host"].toString().isEmpty() || m["host"].toString() != QSysInfo::machineHostName())
        return false;
    const auto boot = bootId();
    if (!boot.isEmpty() && !m["boot_id"].toString().isEmpty() && m["boot_id"].toString() != boot)
        return true;
    const qint64 pid = m["pid"].toInteger();
    if (pid <= 0)
        return false;
    if (::kill(pid_t(pid), 0) == -1 && errno == ESRCH)
        return true;
    const auto start = processStart(pid);
    return !start.isEmpty() && !m["pid_start"].toString().isEmpty() && start != m["pid_start"].toString();
#elif defined(Q_OS_WIN)
    if (m["host"].toString().isEmpty() || m["host"].toString() != QSysInfo::machineHostName())
        return false;
    const qint64 pid = m["pid"].toInteger();
    return pid > 0 && !windowsProcessRunning(pid);
#else
    Q_UNUSED(m);
    return false;
#endif
}
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
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>

#include <tlhelp32.h> // After windows.h.
namespace mf {
namespace {
bool windowsProcessRunning(qint64 pid) {
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!handle)
        return GetLastError() == ERROR_ACCESS_DENIED; // Exists, but belongs to someone else.
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}
// Creation time of a process, or 0 when it cannot be opened.
quint64 windowsProcessCreation(DWORD pid) {
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle)
        return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(handle, &created, &exited, &kernel, &user);
    CloseHandle(handle);
    return ok ? (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
void windowsTerminateTree(qint64 pid) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    QMultiHash<DWORD, DWORD> children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (bool more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID != entry.th32ParentProcessID)
            children.insert(entry.th32ParentProcessID, entry.th32ProcessID);
    CloseHandle(snapshot);
    // A parent id may have been recycled: only processes started after the run belong to its tree.
    const auto started = windowsProcessCreation(DWORD(pid));
    QList<DWORD> tree{DWORD(pid)};
    for (qsizetype i = 0; i < tree.size(); ++i)
        for (const auto child : children.values(tree[i]))
            if (!tree.contains(child) && windowsProcessCreation(child) >= started)
                tree << child;
    // Parents first, so that none of them starts a replacement for an ended child.
    for (const auto id : tree)
        if (const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, id)) {
            TerminateProcess(handle, 1);
            CloseHandle(handle);
        }
}
} // namespace
} // namespace mf
#endif
