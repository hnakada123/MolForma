// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_execution.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/run_diagnostics.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
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
QString untrustedNetworkPath(const Document &d) {
    const auto s = executionSettings(d), preferred = executionPreferences();
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
const QStringList bundleFiles{"input.nw", "project.molforma.json", "run.json", "output.out", "stderr.err"};
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
void writeManifest(const ExecutionPlan &plan) {
    QSaveFile file(QDir(plan.directory).filePath("run.json"));
    const auto bytes = QJsonDocument(plan.manifest).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(tr("実行記録を保存できません: %1").arg(file.errorString()));
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
    return QDir(directory).filePath("stop.request");
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
        manifest = parseJson(readFile(QDir(directory).filePath("run.json")));
    } catch (const std::exception &) {
        return false;
    }
    if (!runStateActive(runStateFromKey(manifest["status"].toString())) || !runSupervised(manifest))
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
        const auto manifest = parseJson(readFile(QDir(directory).filePath("run.json")));
        const auto input = readFile(QDir(directory).filePath("input.nw"));
        if (manifest["format"] != "molforma-nwchem-run" ||
            runStateFromKey(manifest["status"].toString()) != RunState::Starting ||
            manifest["input_sha256"].toString() !=
                QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()))
            return 2;
        plan.directory = directory;
        plan.program = manifest["program"].toString();
        for (const auto &argument : manifest["arguments"].toArray())
            plan.arguments << argument.toString();
        const auto overrides = manifest["environment_overrides"].toObject();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            plan.environment[it.key()] = it.value().toString();
        plan.timeoutSeconds = manifest["timeout_seconds"].toInt();
        plan.manifest = manifest;
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
            {"job_queue", ""}};
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
    return {"executable",    "mode",     "launcher",        "mpi_ranks",   "omp_threads", "blas_threads",
            "omp_stacksize", "job_root", "timeout_seconds", "environment", "job_script",  "job_queue"};
}
QJsonObject executionPreferences() {
    const auto stored =
        QJsonDocument::fromJson(QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                                    .value("execution/defaults")
                                    .toByteArray())
            .object();
    const auto defaults = executionDefaults();
    QJsonObject result;
    for (const auto &key : executionPreferenceKeys())
        result[key] = stored[key].type() == defaults[key].type() ? stored[key] : defaults[key];
    return result;
}
void saveExecutionPreferences(const QJsonObject &settings) {
    QJsonObject stored;
    for (const auto &key : executionPreferenceKeys())
        if (settings.contains(key))
            stored[key] = settings[key];
    QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
        .setValue("execution/defaults", QJsonDocument(stored).toJson(QJsonDocument::Compact));
}
void applyExecutionPreferences(Document &d) {
    const auto defaults = executionDefaults(), preferences = executionPreferences();
    for (auto it = preferences.begin(); it != preferences.end(); ++it)
        if (it.value() != defaults[it.key()])
            setExecutionSetting(d, it.key(), it.value());
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
ExecutionValidation validateExecution(const Document &d, const QString &base, bool local) {
    ExecutionValidation r;
    const auto s = executionSettings(d);
    const auto calculation = validateNwchem(d);
    static_cast<ValidationReport &>(r) = calculation;
    if (d.data.contains("nwchem_execution") && !d.data["nwchem_execution"].isObject())
        r.error("execution.object", "nwchem_execution",
                tr("NWChem実行設定はオブジェクトである必要があります。"), {}, 2);
    const auto defaults = executionDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto v = s[it.key()];
        if (v.type() != it.value().type())
            r.error("execution.type", it.key(), tr("実行設定の型が不正です: %1").arg(it.key()), {}, 2);
        else if (v.isString() && (controls(v.toString()) || v.toString().size() > 4096))
            r.error("execution.string", it.key(),
                    tr("実行設定に制御文字または長すぎる文字列があります: %1").arg(it.key()), {}, 2);
    }
    auto integer = [&](const QString &key, int lo, int hi) {
        const double n = s[key].toDouble(-1);
        if (!s[key].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < lo || n > hi)
            r.error("execution.integer", key,
                    tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(lo).arg(hi), {}, 2);
    };
    const bool mpi = s["mode"] == "mpi";
    if (!mpi && s["mode"] != "direct")
        r.error("execution.mode", "mode", tr("起動方式はMPIまたは直接実行を選んでください。"), {}, 2);
    for (const auto &range : executionRanges())
        integer(range.key, range.minimum, range.maximum);
    for (const auto &key : {"executable", "job_root"})
        if (s[key].toString().trimmed().isEmpty())
            r.error("execution.required", key, tr("%1 を指定してください。").arg(key), {}, 2);
    if (mpi && s["launcher"].toString().trimmed().isEmpty())
        r.error("execution.launcher", "launcher", tr("MPIランチャーを指定してください。"), {}, 2);
    if (!QStringList{"none", "shell", "slurm", "pbs"}.contains(s["job_script"].toString()))
        r.error("execution.job_script", "job_script",
                tr("ジョブスクリプトは、作らない・シェル・Slurm・PBSから選んでください。"), {}, 2);
    if (!QRegularExpression("^[A-Za-z0-9_.@:+-]{0,64}$").match(s["job_queue"].toString()).hasMatch())
        r.error("execution.job_queue", "job_queue",
                tr("キュー・パーティション名は英数字と _ . @ : + - の64文字以内で指定してください。"), {}, 2);
    const auto stack = s["omp_stacksize"].toString();
    if (!stack.isEmpty() && !QRegularExpression("^[1-9][0-9]{0,8}[BKMGbkmg]?$").match(stack).hasMatch())
        r.error("execution.stack", "omp_stacksize",
                tr("OpenMPスタックは64M・1G等の正の整数と単位B/K/M/Gで指定してください。"), {}, 2);
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
                    tr("環境変数の行に有効・無効の指定がありません。"), {}, 2);
            continue;
        }
        if (!row["enabled"].toBool())
            continue;
        const auto name = row["name"].toString(), value = row["value"].toString();
        if (!row["name"].isString() ||
            !QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(name).hasMatch() ||
            !row["value"].isString() || controls(value) || value.size() > 8192) {
            r.error("execution.environment_value", "environment",
                    tr("有効な環境変数の名前・値を確認してください。"), {}, 2);
            continue;
        }
        if (seen.contains(name) || managedEnvironment.contains(name)) {
            r.error("execution.environment_duplicate", "environment",
                    tr("環境変数が重複、または専用の資源設定と重複しています: %1").arg(name), {}, 2);
            continue;
        }
        seen.insert(name);
        r.environment[name] = value;
    }
    if (r.environment.contains("NWCHEM_BASIS_LIBRARY") && !r.environment["NWCHEM_BASIS_LIBRARY"].isEmpty())
        r.environment["NWCHEM_BASIS_LIBRARY"] =
            absolutePath(r.environment["NWCHEM_BASIS_LIBRARY"], base) + '/';
    auto environment = QProcessEnvironment::systemEnvironment();
    for (auto it = r.environment.begin(); it != r.environment.end(); ++it)
        environment.insert(it.key(), it.value());
    r.referenceDirectory = s["reference_directory"].toString().isEmpty()
                               ? absolutePath(".", base)
                               : absolutePath(s["reference_directory"].toString(), base);
    r.executable = locateProgram(s["executable"].toString(), base, environment);
    r.launcher = locateProgram(s["launcher"].toString(), base, environment);
    if (const auto network = untrustedNetworkPath(d); !network.isEmpty()) {
        r.error(
            "execution.network", "job_root",
            tr("ネットワーク上の場所 %1 は、「編集 → 環境設定」で指定した場合だけ使えます。").arg(network),
            {}, 2);
        local = false; // Not even looked at.
    }
    if (local) {
        const auto check = [&](const QString &key, const QString &label, bool ok, const QString &found,
                               const QString &error) {
            r.checks << ExecutionCheck{label, ok ? found : error, ok ? QString() : error, ok};
            if (!ok)
                r.error("execution.local", key, error, {}, 2);
        };
        const auto program = [&](const QString &key, const QString &path, const QString &label,
                                 const QString &error) {
            const QFileInfo f(path);
            check(key, label, !path.isEmpty() && f.isFile() && f.isExecutable(), path, error);
        };
        program("executable", r.executable, tr("NWChem実行ファイル"),
                tr("NWChem実行ファイルが見つからないか、実行権限がありません。"));
        if (mpi)
            program("launcher", r.launcher, tr("MPIランチャー"),
                    tr("MPIランチャーが見つからないか、実行権限がありません。"));
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
        if (!basis.isEmpty())
            check("environment", "NWCHEM_BASIS_LIBRARY", QFileInfo(basis).isDir(), basis,
                  tr("NWCHEM_BASIS_LIBRARYのディレクトリがありません。"));
        // The input's own errors stay in the list; the check only counts them.
        r.checks << ExecutionCheck{tr("NWChem入力"),
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
                  {}, 2);
    if (!mpi)
        r.warning("execution.direct", "mode",
                  tr("直接実行は対応する逐次版等に使用してください。MPI版はMPIランチャーを選んでください。"),
                  {}, 2);
    if (ranks * std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt()) > QThread::idealThreadCount())
        r.warning("execution.cpus", "mpi_ranks", tr("指定した並列度が、このPCの論理CPU数を超えています。"),
                  {}, 2);
    if (s["omp_threads"].toInt() > 1 && s["blas_threads"].toInt() > 1)
        r.warning("execution.threads", "omp_threads",
                  tr("OpenMPとBLASの多重並列により、想定より多くのスレッドを使う場合があります。"), {}, 2);
    for (auto it = s.begin(); it != s.end(); ++it)
        if (!defaults.contains(it.key()))
            r.warning("execution.unknown", it.key(), tr("未対応の実行設定を保持しています: %1").arg(it.key()),
                      {}, 2);
    r.errors.removeDuplicates();
    return r;
}
ExecutionPlan executionPlan(const Document &d, const QString &base, const QString &directory, bool local) {
    const auto report = validateExecution(d, base, local);
    if (!report.errors.isEmpty())
        throw ValidationFailure(report);
    const auto s = executionSettings(d);
    ExecutionPlan p;
    p.directory = QDir::cleanPath(directory);
    p.timeoutSeconds = s["timeout_seconds"].toInt();
    p.environment = report.environment;
    const QString executable = report.executable.isEmpty() ? s["executable"].toString() : report.executable;
    p.program = executable;
    if (s["mode"] == "mpi") {
        p.program = report.launcher.isEmpty() ? s["launcher"].toString() : report.launcher;
        p.arguments = {"-np", QString::number(s["mpi_ranks"].toInt()), executable};
    }
    p.arguments << "input.nw";
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
            if (local && !nwchemRestart(nwchemSettings(d)).active() &&
                (QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
                fail(tr("出力movecsは既に存在します。別の名前を指定してください: %1").arg(target));
            if (local && QDir::isAbsolutePath(actual) &&
                !writableDirectory(QFileInfo(target).absolutePath(), false))
                fail(tr("出力movecsの保存先がないか、書込みできません: %1").arg(target));
        } else if (local && absolute) {
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
    p.manifest = {{"format", "molforma-nwchem-run"},
                  {"version", 1},
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
                  {"input", "input.nw"},
                  {"stdout", "output.out"},
                  {"stderr", "stderr.err"},
                  {"project", "project.molforma.json"},
                  {"timeout_seconds", p.timeoutSeconds},
                  {"input_sha256",
                   QString::fromLatin1(
                       QCryptographicHash::hash(p.input.toUtf8(), QCryptographicHash::Sha256).toHex())}};
    if (!outputs.isEmpty())
        p.manifest["visualization_files"] = QJsonArray::fromStringList(outputs);
    return p;
}
ExecutionPlan executionPreviewPlan(const Document &d, const QString &base, bool local) {
    const auto root = executionRoot(executionSettings(d), base);
    const auto job = QDir(root).filePath(nwchemSettings(d)["start"].toString() + "-YYYYMMDD-HHMMSS-ID");
    return executionPlan(d, base, job, local);
}
QString executionCommand(const ExecutionPlan &p) {
    QString command = "cd -- " + quote(p.directory) + "\nenv";
    for (auto it = p.environment.begin(); it != p.environment.end(); ++it)
        command += ' ' + quote(it.key() + '=' + it.value());
    command += ' ' + quote(p.program);
    for (const auto &arg : p.arguments)
        command += ' ' + quote(arg);
    return command + " > output.out 2> stderr.err";
}
QString executionJobPrelude() {
    return QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
        .value("execution/job_prelude")
        .toString();
}
void saveExecutionJobPrelude(const QString &lines) {
    if (lines.size() > 4000 || lines.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]")))
        fail(tr("ジョブスクリプトの準備の行は、制御文字を含めず4000文字以内で指定してください。"));
    QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
        .setValue("execution/job_prelude", lines.trimmed());
}
QString executionJobScript(const Document &d) {
    const auto s = executionSettings(d);
    const auto kind = s["job_script"].toString();
    if (kind == "none")
        return {};
    const auto report = validateExecution(d, QDir::homePath(), false);
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
    const auto name = nwchemSettings(d)["start"].toString(), queue = s["job_queue"].toString();
    QStringList lines{"#!/bin/bash"};
    if (kind == "slurm") {
        lines << "#SBATCH --job-name=" + name << "#SBATCH --nodes=1"
              << QString("#SBATCH --ntasks=%1").arg(ranks)
              << QString("#SBATCH --cpus-per-task=%1").arg(threads);
        if (limit)
            lines << "#SBATCH --time=" + time;
        if (!queue.isEmpty())
            lines << "#SBATCH --partition=" + queue;
        lines << "#SBATCH --output=scheduler-%j.log";
    } else if (kind == "pbs") {
        lines << "#PBS -N " + name
              << QString("#PBS -l select=1:ncpus=%1:mpiprocs=%2:ompthreads=%3")
                     .arg(ranks * threads)
                     .arg(ranks)
                     .arg(s["omp_threads"].toInt());
        if (limit)
            lines << "#PBS -l walltime=" + time;
        if (!queue.isEmpty())
            lines << "#PBS -q " + queue;
        lines << "#PBS -j oe";
    }
    lines << "# Generated by MolForma. Runs NWChem in the folder of this script; copy the folder back and"
          << "# register output.out in MolForma's history afterwards.";
    lines << (kind == "slurm" ? "cd \"${SLURM_SUBMIT_DIR:-$(dirname \"$0\")}\" || exit 1"
              : kind == "pbs" ? "cd \"${PBS_O_WORKDIR:-$(dirname \"$0\")}\" || exit 1"
                              : "cd \"$(dirname \"$0\")\" || exit 1");
    if (const auto prelude = executionJobPrelude(); !prelude.isEmpty())
        lines << prelude;
    for (auto it = report.environment.begin(); it != report.environment.end(); ++it) {
        // The resources are this job's; other variables name places of the computer they were set on.
        const auto line = "export " + it.key() + '=' + quote(it.value());
        lines << (managedEnvironment.contains(it.key()) ? line : "# " + line);
    }
    // The programs as they are named in the settings, not where this computer found them.
    QStringList command;
    if (mpi)
        command << quote(s["launcher"].toString()) << "-np" << QString::number(ranks);
    command << quote(s["executable"].toString()) << "input.nw";
    lines << command.join(' ') + " > output.out 2> stderr.err";
    return lines.join('\n') + '\n';
}
ExecutionPlan saveExecutionBundle(const Document &d, const QString &base, bool forRun) {
    const auto report = validateExecution(d, base, forRun);
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
    auto plan = executionPlan(d, base, directory.path(), forRun);
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
    writeFile(directory.filePath("input.nw"), plan.input.toUtf8());
    const auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    const auto project = QJsonDocument(d.project()).toJson();
    writeFile(directory.filePath("project.molforma.json"), project);
    plan.manifest["project_sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(project, QCryptographicHash::Sha256).toHex());
    copyRestartFiles(d, directory.path());
    // A job saved to run elsewhere takes its start script along.
    if (const auto script = forRun ? QString() : executionJobScript(d); !script.isEmpty()) {
        const auto path = directory.filePath("job.sh");
        writeFile(path, script.toUtf8());
        QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeUser);
        plan.manifest["job_script"] = "job.sh";
    }
    writeManifest(plan);
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
    if (const auto started =
            QDateTime::fromString(current.manifest["started_at"].toString(), Qt::ISODateWithMs);
        watching && started.isValid())
        return std::max<qint64>(0, started.secsTo(QDateTime::currentDateTimeUtc()));
    return elapsed.elapsed() / 1000;
}
void NwchemRun::start(const Document &d, const QString &base) {
    if (active())
        fail(tr("計算は既に実行中です。"));
    startPrepared(saveExecutionBundle(d, base, true));
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
    elapsed.start();
    if (!supervisor.isEmpty()) {
        launch();
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
    process.setStandardOutputFile(QDir(current.directory).filePath("output.out"));
    process.setStandardErrorFile(QDir(current.directory).filePath("stderr.err"));
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
        manifest = parseJson(readFile(QDir(job).filePath("run.json")));
    } catch (const std::exception &) {
        return false;
    }
    const auto state = runStateFromKey(manifest["status"].toString());
    if (manifest["format"] != "molforma-nwchem-run" || !runStateActive(state) || !runSupervised(manifest))
        return false;
    current = {};
    current.directory = job;
    current.program = manifest["program"].toString();
    for (const auto &argument : manifest["arguments"].toArray())
        current.arguments << argument.toString();
    current.timeoutSeconds = manifest["timeout_seconds"].toInt();
    current.manifest = manifest;
    status = state;
    detail = manifest["error"].toString();
    timedOut = false;
    watching = true;
    supervisorPid = unreadable = 0;
    groupId = duration = 0;
    timer.start();
    emit changed();
    return true;
}
void NwchemRun::poll() {
    QJsonObject manifest;
    try {
        manifest = parseJson(readFile(QDir(current.directory).filePath("run.json")));
    } catch (const std::exception &) {
        manifest = {};
    }
    auto state = runStateFromKey(manifest["status"].toString());
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
            manifest = parseJson(readFile(QDir(current.directory).filePath("run.json")));
            if (runStateActive(runStateFromKey(manifest["status"].toString())) &&
                !manifest.contains("supervisor")) {
                manifest["supervisor"] = executionProcessIdentity(supervisorPid);
                auto plan = current;
                plan.manifest = manifest;
                writeManifest(plan);
            }
        } catch (const std::exception &) {
        }
        state = runStateFromKey(manifest["status"].toString());
    }
    if (runStateActive(state) && executionRunGone(manifest)) {
        // The supervisor ended without recording the end, and NWChem is not running either. Its last
        // word may have come between the reading and the look at the processes: the repair reads again.
        try {
            repairHistoryEntry(current.directory);
        } catch (const std::exception &) {
        }
        try {
            manifest = parseJson(readFile(QDir(current.directory).filePath("run.json")));
        } catch (const std::exception &) {
        }
        state = runStateFromKey(manifest["status"].toString());
        if (state == RunState::Invalid)
            state = RunState::Interrupted;
    }
    // The stop this object asked for shows before the supervisor has recorded it.
    const auto shown = status == RunState::Stopping && runStateActive(state) ? RunState::Stopping : state;
    current.manifest = manifest;
    status = shown;
    detail = manifest["error"].toString();
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
    current.manifest["exit_code"] = code;
    current.manifest["exit_status"] = exitStatus == QProcess::NormalExit ? "normal" : "crash";
    current.manifest["finished_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    try { // The whole log, once: the history list then needs no log reading.
        current.manifest["summary"] = nwchemLogSummary(QDir(current.directory).filePath("output.out"));
    } catch (const std::exception &) {
        current.manifest.remove("summary");
    }
    QString output, errors;
    try {
        output = historyText(current.directory, "output.out", true);
    } catch (const std::exception &) {
    }
    try {
        errors = historyText(current.directory, "stderr.err", true);
    } catch (const std::exception &) {
    }
    current.manifest["diagnostics"] = diagnoseRun(status, output, errors, error);
    record();
    emit changed();
}
void NwchemRun::record() {
    current.manifest["status"] = runStateKey(status);
    current.manifest["elapsed_seconds"] = elapsedSeconds();
    current.manifest["error"] = detail;
    try {
        writeManifest(current);
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
