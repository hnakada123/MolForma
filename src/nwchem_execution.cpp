#include "nwchem_execution.h"
#include "nwchem.h"
#include "nwchem_advanced.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace mf {
namespace {
const QStringList managedEnvironment{"OMP_NUM_THREADS",      "OMP_STACKSIZE",    "OMP_DYNAMIC",
                                     "OPENBLAS_NUM_THREADS", "GOTO_NUM_THREADS", "MKL_NUM_THREADS",
                                     "MKL_DYNAMIC",          "BLIS_NUM_THREADS", "VECLIB_MAXIMUM_THREADS"};
const QStringList bundleFiles{"input.nw", "project.kgui.json", "run.json", "output.out", "stderr.err"};
bool controls(const QString &s) {
    return s.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
QString absolutePath(QString path, const QString &base) {
    if (path == "~")
        path = QDir::homePath();
    else if (path.startsWith("~/"))
        path = QDir::home().filePath(path.mid(2));
    return QDir::cleanPath(QDir(base).absoluteFilePath(path));
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
        fail("実行記録を保存できません: " + file.errorString());
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
        r.errors << "NWChem実行設定はオブジェクトである必要があります。";
    const auto defaults = executionDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto v = s[it.key()];
        if (v.type() != it.value().type())
            r.errors << "実行設定の型が不正です: " + it.key();
        else if (v.isString() && (controls(v.toString()) || v.toString().size() > 4096))
            r.errors << "実行設定に制御文字または長すぎる文字列があります: " + it.key();
    }
    auto integer = [&](const QString &key, int lo, int hi) {
        const double n = s[key].toDouble(-1);
        if (!s[key].isDouble() || !std::isfinite(n) || std::floor(n) != n || n < lo || n > hi)
            r.errors << QString("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(lo).arg(hi);
    };
    const bool mpi = s["mode"] == "mpi";
    if (!mpi && s["mode"] != "direct")
        r.errors << "起動方式はMPIまたは直接実行を選んでください。";
    integer("mpi_ranks", 1, 4096);
    integer("omp_threads", 1, 1024);
    integer("blas_threads", 1, 1024);
    integer("timeout_seconds", 0, 604800);
    for (const auto &key : {"executable", "job_root"})
        if (s[key].toString().trimmed().isEmpty())
            r.errors << QString("%1 を指定してください。").arg(key);
    if (mpi && s["launcher"].toString().trimmed().isEmpty())
        r.errors << "MPIランチャーを指定してください。";
    const auto stack = s["omp_stacksize"].toString();
    if (!stack.isEmpty() && !QRegularExpression("^[1-9][0-9]{0,8}[BKMGbkmg]?$").match(stack).hasMatch())
        r.errors << "OpenMPスタックは64M・1G等の正の整数と単位B/K/M/Gで指定してください。";
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
            r.errors << "環境変数の行に有効・無効の指定がありません。";
            continue;
        }
        if (!row["enabled"].toBool())
            continue;
        const auto name = row["name"].toString(), value = row["value"].toString();
        if (!row["name"].isString() ||
            !QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(name).hasMatch() ||
            !row["value"].isString() || controls(value) || value.size() > 8192) {
            r.errors << "有効な環境変数の名前・値を確認してください。";
            continue;
        }
        if (seen.contains(name) || managedEnvironment.contains(name)) {
            r.errors << "環境変数が重複、または専用の資源設定と重複しています: " + name;
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
        auto checkProgram = [&](const QString &path, const QString &label) {
            const QFileInfo f(path);
            if (path.isEmpty() || !f.isFile() || !f.isExecutable())
                r.errors << label + "が見つからないか、実行権限がありません。";
        };
        checkProgram(r.executable, "NWChem実行ファイル");
        if (mpi)
            checkProgram(r.launcher, "MPIランチャー");
        if (!QFileInfo(r.referenceDirectory).isDir())
            r.errors << "参照ファイルの基準ディレクトリがありません。";
        if (!writableDirectory(executionRoot(s, base), true))
            r.errors << "ジョブ保存先にディレクトリを作成できません。";
        const auto basis = r.environment.value("NWCHEM_BASIS_LIBRARY");
        if (!basis.isEmpty() && !QFileInfo(basis).isDir())
            r.errors << "NWCHEM_BASIS_LIBRARYのディレクトリがありません。";
    }
    const int ranks = mpi ? s["mpi_ranks"].toInt() : 1;
    if (mpi && ranks == 1)
        r.warnings << "MPI-PR版NWChemは補助プロセスを必要とします。1計算プロセスでもMPI起動数2が必要です。";
    if (!mpi)
        r.warnings << "直接実行は対応する逐次版等に使用してください。MPI版はMPIランチャーを選んでください。";
    if (ranks * std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt()) > QThread::idealThreadCount())
        r.warnings << "指定した並列度が、このPCの論理CPU数を超えています。";
    if (s["omp_threads"].toInt() > 1 && s["blas_threads"].toInt() > 1)
        r.warnings << "OpenMPとBLASの多重並列により、想定より多くのスレッドを使う場合があります。";
    for (auto it = s.begin(); it != s.end(); ++it)
        if (!defaults.contains(it.key()))
            r.warnings << "未対応の実行設定を保持しています: " + it.key();
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
            fail("入力movecsを読めません: " + input);
        setNwchemAdvanced(resolved, "vectors_input", input);
    }
    for (const auto &key : {"scratch_dir", "permanent_dir", "vectors_output"}) {
        if (!advanced.contains(key))
            continue;
        const auto value = advanced[key].toString();
        const bool absolute = QDir::isAbsolutePath(value);
        const auto clean = QDir::cleanPath(value);
        if (!absolute && (clean == ".." || clean.startsWith("../")))
            fail(QString("%1 の相対パスはジョブフォルダー内を指定してください。").arg(key));
        const auto target = absolutePath(value, directory);
        if (QString(key) == "vectors_output") {
            if (!absolute && bundleFiles.contains(clean))
                fail("出力movecsにはジョブ保存用ファイルと異なる名前を指定してください。");
            if (local && (QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
                fail("出力movecsは既に存在します。別の名前を指定してください: " + target);
            if (local && absolute && !writableDirectory(QFileInfo(target).absolutePath(), false))
                fail("出力movecsの保存先がないか、書込みできません: " + target);
        } else if (local && absolute) {
            if (!writableDirectory(target, false))
                fail("指定されたNWChemディレクトリがないか、書込みできません: " + target);
            const auto prefix = nwchemSettings(d)["start"].toString();
            if (!QDir(target)
                     .entryList({prefix + ".*", prefix + "_opt*", prefix + "_vib*"},
                                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                     .isEmpty())
                fail("同じ計算名のファイルが指定ディレクトリにあります。計算名か保存先を変更してください: " +
                     target);
        }
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
                  {"input", "input.nw"},
                  {"stdout", "output.out"},
                  {"stderr", "stderr.err"},
                  {"project", "project.kgui.json"},
                  {"timeout_seconds", p.timeoutSeconds},
                  {"input_sha256",
                   QString::fromLatin1(
                       QCryptographicHash::hash(p.input.toUtf8(), QCryptographicHash::Sha256).toHex())}};
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
        fail("ジョブ保存先を作成できません: " + root);
    QTemporaryDir directory(QDir(root).filePath(nwchemSettings(d)["start"].toString() + '-' +
                                                QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
                                                "-XXXXXX"));
    if (!directory.isValid())
        fail("ジョブフォルダーを作成できません: " + directory.errorString());
    auto plan = executionPlan(d, base, directory.path(), forRun);
    const auto advanced = nwchemAdvanced(d);
    for (const auto &key : {"scratch_dir", "permanent_dir", "vectors_output"}) {
        if (!advanced.contains(key))
            continue;
        QString path = advanced[key].toString();
        if (QDir::isAbsolutePath(path))
            continue;
        if (QString(key) == "vectors_output")
            path = QFileInfo(path).path();
        if (!QDir(directory.path()).mkpath(path))
            fail("ジョブ内の出力ディレクトリを作成できません: " + path);
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
        fail("計算は既に実行中です。");
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
