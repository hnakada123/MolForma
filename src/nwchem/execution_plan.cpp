// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/execution_plan.h"
#include "foundation/error.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/execution_settings.h"
#include "nwchem/job_script.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/remote_host.h"
#include "nwchem/remote_run.h"
#include "nwchem/run_record.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace mf {
using namespace execution_detail;
namespace {
const QStringList bundleFiles{JobFile::input,  JobFile::project, JobFile::record,  JobFile::output,
                              JobFile::errors, JobFile::script,  JobFile::exitCode};
} // namespace
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
        command += jobSubmitCommand(host.scheduler) + '\'';
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
} // namespace mf
