#include "nwchem/nwchem_execution.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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
namespace {
const QStringList managedEnvironment{"OMP_NUM_THREADS",      "OMP_STACKSIZE",    "OMP_DYNAMIC",
                                     "OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS",
                                     "MKL_DYNAMIC",          "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS"};
const QStringList bundleFiles{"input.nw", "project.kgui.json", "run.json", "output.out", "stderr.err"};
bool controls(const QString &s) {
    return s.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
QString locateProgram(const QString &name, const QString &base, const QProcessEnvironment &environment) {
    if (QDir::isAbsolutePath(name) || name.contains('/') || name.startsWith("~"))
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
} // namespace
QJsonObject executionDefaults() {
    return {{"executable", "nwchem"},     {"mode", "mpi"},
            {"launcher", "mpirun"},       {"mpi_ranks", 2},
            {"omp_threads", 1},           {"blas_threads", 1},
            {"omp_stacksize", ""},        {"job_root", QDir::home().filePath("MolForma/jobs")},
            {"reference_directory", ""},  {"timeout_seconds", 0},
            {"environment", QJsonArray{}}};
}
QStringList executionPreferenceKeys() {
    return {"executable",   "mode",          "launcher", "mpi_ranks",       "omp_threads",
            "blas_threads", "omp_stacksize", "job_root", "timeout_seconds", "environment"};
}
QJsonObject executionPreferences() {
    const auto stored =
        QJsonDocument::fromJson(QSettings("MolForma", "MolForma").value("execution/defaults").toByteArray())
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
    QSettings("MolForma", "MolForma")
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
    r.errors = calculation.errors;
    r.warnings = calculation.warnings;
    if (d.data.contains("nwchem_execution") && !d.data["nwchem_execution"].isObject())
        r.errors << tr("NWChem実行設定はオブジェクトである必要があります。");
    const auto defaults = executionDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto v = s[it.key()];
        if (v.type() != it.value().type())
            r.errors << tr("実行設定の型が不正です: %1").arg(it.key());
        else if (v.isString() && (controls(v.toString()) || v.toString().size() > 4096))
            r.errors << tr("実行設定に制御文字または長すぎる文字列があります: %1").arg(it.key());
    }
    auto integer = [&](const QString &key, int lo, int hi) {
        const double n = s[key].toDouble(-1);
        if (!s[key].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < lo || n > hi)
            r.errors << tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(lo).arg(hi);
    };
    const bool mpi = s["mode"] == "mpi";
    if (!mpi && s["mode"] != "direct")
        r.errors << tr("起動方式はMPIまたは直接実行を選んでください。");
    integer("mpi_ranks", 1, 4096);
    integer("omp_threads", 1, 1024);
    integer("blas_threads", 1, 1024);
    integer("timeout_seconds", 0, 604800);
    for (const auto &key : {"executable", "job_root"})
        if (s[key].toString().trimmed().isEmpty())
            r.errors << tr("%1 を指定してください。").arg(key);
    if (mpi && s["launcher"].toString().trimmed().isEmpty())
        r.errors << tr("MPIランチャーを指定してください。");
    const auto stack = s["omp_stacksize"].toString();
    if (!stack.isEmpty() && !QRegularExpression("^[1-9][0-9]{0,8}[BKMGbkmg]?$").match(stack).hasMatch())
        r.errors << tr("OpenMPスタックは64M・1G等の正の整数と単位B/K/M/Gで指定してください。");
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
            r.errors << tr("環境変数の行に有効・無効の指定がありません。");
            continue;
        }
        if (!row["enabled"].toBool())
            continue;
        const auto name = row["name"].toString(), value = row["value"].toString();
        if (!row["name"].isString() ||
            !QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(name).hasMatch() ||
            !row["value"].isString() || controls(value) || value.size() > 8192) {
            r.errors << tr("有効な環境変数の名前・値を確認してください。");
            continue;
        }
        if (seen.contains(name) || managedEnvironment.contains(name)) {
            r.errors << tr("環境変数が重複、または専用の資源設定と重複しています: %1").arg(name);
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
    if (local) {
        const auto check = [&](const QString &label, bool ok, const QString &found, const QString &error) {
            r.checks << ExecutionCheck{label, ok ? found : error, ok ? QString() : error, ok};
            if (!ok)
                r.errors << error;
        };
        const auto program = [&](const QString &path, const QString &label, const QString &error) {
            const QFileInfo f(path);
            check(label, !path.isEmpty() && f.isFile() && f.isExecutable(), path, error);
        };
        program(r.executable, tr("NWChem実行ファイル"),
                tr("NWChem実行ファイルが見つからないか、実行権限がありません。"));
        if (mpi)
            program(r.launcher, tr("MPIランチャー"),
                    tr("MPIランチャーが見つからないか、実行権限がありません。"));
        check(tr("参照ファイルの基準"), QFileInfo(r.referenceDirectory).isDir(), r.referenceDirectory,
              tr("参照ファイルの基準ディレクトリがありません。"));
        const auto root = executionRoot(s, base);
        check(tr("ジョブ保存先"), writableDirectory(root, true), root,
              tr("ジョブ保存先にディレクトリを作成できません。"));
        const auto basis = r.environment.value("NWCHEM_BASIS_LIBRARY");
        if (!basis.isEmpty())
            check("NWCHEM_BASIS_LIBRARY", QFileInfo(basis).isDir(), basis,
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
        r.warnings << tr(
            "MPI-PR版NWChemは補助プロセスを必要とします。1計算プロセスでもMPI起動数2が必要です。");
    if (!mpi)
        r.warnings << tr(
            "直接実行は対応する逐次版等に使用してください。MPI版はMPIランチャーを選んでください。");
    if (ranks * std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt()) > QThread::idealThreadCount())
        r.warnings << tr("指定した並列度が、このPCの論理CPU数を超えています。");
    if (s["omp_threads"].toInt() > 1 && s["blas_threads"].toInt() > 1)
        r.warnings << tr("OpenMPとBLASの多重並列により、想定より多くのスレッドを使う場合があります。");
    for (auto it = s.begin(); it != s.end(); ++it)
        if (!defaults.contains(it.key()))
            r.warnings << tr("未対応の実行設定を保持しています: %1").arg(it.key());
    r.errors.removeDuplicates();
    return r;
}
ExecutionPlan executionPlan(const Document &d, const QString &base, const QString &directory, bool local) {
    const auto report = validateExecution(d, base, local);
    if (!report.errors.isEmpty())
        fail(report.errors.join('\n'));
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
            if (local && (QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
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
    p.input = nwchemInput(resolved);
    QJsonObject overrides;
    for (auto it = p.environment.begin(); it != p.environment.end(); ++it)
        overrides[it.key()] = it.value();
    p.manifest = {{"format", "molforma-nwchem-run"},
                  {"version", 1},
                  {"status", "prepared"},
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
                  {"project", "project.kgui.json"},
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
ExecutionPlan saveExecutionBundle(const Document &d, const QString &base, bool forRun) {
    const auto report = validateExecution(d, base, forRun);
    if (!report.errors.isEmpty())
        fail(report.errors.join('\n'));
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
    writeFile(directory.filePath("project.kgui.json"), project);
    plan.manifest["project_sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(project, QCryptographicHash::Sha256).toHex());
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
        if (!cancelling && current.timeoutSeconds && elapsedSeconds() >= current.timeoutSeconds) {
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
        if (cancelling) {
            signalProcess(false);
            killTimer.start();
        } else
            status = "running";
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
    return running ? elapsed.elapsed() / 1000 : duration;
}
void NwchemRun::start(const Document &d, const QString &base) {
    if (running)
        fail(tr("計算は既に実行中です。"));
    current = saveExecutionBundle(d, base, true);
    status = "starting";
    detail.clear();
    cancelling = timedOut = false;
    groupId = 0;
    duration = 0;
    running = true;
    elapsed.start();
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
    if (running)
        timer.start();
    emit changed();
}
void NwchemRun::signalProcess(bool force) {
    // A forked child still inherits the application's signal handlers until exec.
    // Defer graceful cancellation to started(); a hung startup can still be killed.
    if (!running || (!force && process.state() != QProcess::Running))
        return;
#ifdef Q_OS_UNIX
    if (groupId > 0)
        ::kill(-pid_t(groupId), force ? SIGKILL : SIGTERM);
#endif
    if (force)
        process.kill();
    else
        process.terminate();
}
void NwchemRun::stop() {
    if (!running || cancelling)
        return;
    cancelling = true;
    status = "stopping";
    signalProcess(false);
    killTimer.start();
    record();
    emit changed();
}
void NwchemRun::finish(int code, QProcess::ExitStatus exitStatus, const QString &error) {
    if (!running)
        return;
    // Clean up surviving children in our private process group before allowing a new run.
    signalProcess(true);
    duration = elapsed.elapsed() / 1000;
    running = false;
    groupId = 0;
    timer.stop();
    killTimer.stop();
    status = timedOut                                                              ? "timed_out"
             : cancelling                                                          ? "cancelled"
             : !error.isEmpty() || exitStatus != QProcess::NormalExit || code != 0 ? "failed"
                                                                                   : "finished";
    detail = error;
    current.manifest["exit_code"] = code;
    current.manifest["exit_status"] = exitStatus == QProcess::NormalExit ? "normal" : "crash";
    current.manifest["finished_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    try { // The whole log, once: the history list then needs no log reading.
        current.manifest["summary"] = nwchemLogSummary(QDir(current.directory).filePath("output.out"));
    } catch (const std::exception &) {
        current.manifest.remove("summary");
    }
    record();
    emit changed();
}
void NwchemRun::record() {
    current.manifest["status"] = status;
    current.manifest["elapsed_seconds"] = elapsedSeconds();
    current.manifest["error"] = detail;
    try {
        writeManifest(current);
    } catch (const std::exception &e) {
        detail = QString::fromUtf8(e.what());
    }
}
void NwchemRun::shutdown() {
    if (!running)
        return;
    stop();
    if (!process.waitForFinished(2000)) {
        signalProcess(true);
        process.waitForFinished(2000);
    }
}
} // namespace mf
