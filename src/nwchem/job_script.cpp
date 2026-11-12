// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/job_script.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "nwchem/execution_detail.h"
#include "nwchem/execution_settings.h"
#include "nwchem/nwchem.h"
#include "nwchem/remote_host.h"
#include "nwchem/run_record.h"
#include <QDir>
#include <QRegularExpression>

namespace mf {
namespace execution_detail {
// How job.sh and the command shown for a run send NWChem's output and errors to their files.
QString outputRedirection() {
    return QString(" > %1 2> %2").arg(QString(JobFile::output), QString(JobFile::errors));
}
} // namespace execution_detail
using namespace execution_detail;
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
QString jobSubmitCommand(const QString &scheduler) {
    return scheduler == "slurm" ? QString("sbatch %1").arg(QString(JobFile::script))
           : scheduler == "pbs" ? QString("qsub %1").arg(QString(JobFile::script))
                                : QString("nohup ./%1 &").arg(QString(JobFile::script));
}
QString remoteJobPreview(const Document &d, const ExecutionEnvironment &environment) {
    const auto name = executionSettings(d)["remote"].toString();
    const auto host = environment.remote(name);
    if (!host)
        fail(tr("実行先「%1」はこのPCの環境設定に登録されていません。").arg(name));
    const QString submit = jobSubmitCommand(host->scheduler);
    const auto where = host->host + ':' + host->directory + '/' + nwchemSettings(d)["start"].toString() +
                       "-YYYYMMDD-HHMMSS-ID/";
    return tr("# ssh・scpで %1 "
              "へ複写し、そこで「%2」として投入します。終わると同じジョブフォルダーへ回収します。")
               .arg(where, submit) +
           '\n' + executionJobScript(d, &*host, environment);
}
} // namespace mf
