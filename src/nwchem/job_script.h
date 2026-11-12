// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/execution_environment.h"
#include <QMap>
#include <QString>
#include <QStringList>

namespace mf {
// The header of a job script for a scheduler (slurm, pbs): a template of lines starting with "#" whose
// placeholders ({name}, {ranks}, …) take the values of the calculation. A line with a placeholder that
// has no value ({time} without a time limit, {queue} without a queue) is left out. The user's own
// templates are kept in Preferences (jobHeaderTemplate in execution_environment.h).
QStringList jobHeaderPlaceholders();
QString defaultJobHeader(const QString &scheduler);
QStringList jobHeaderErrors(const QString &text);
QStringList renderJobHeader(const QString &text, const QMap<QString, QString> &values);
// The script "ジョブとして保存" puts beside the input (job.sh) for running the job on another computer:
// a plain shell script, or one for sbatch (Slurm) or qsub (PBS) with the header of its template
// (the environment's jobHeader), by the setting "job_script"; empty for "none". It starts NWChem as
// "3 実行設定" names it, in the folder of the script, after the environment's preparation lines, and
// leaves NWChem's exit code in the file exit_code. With a remote destination, the script of a run there:
// its scheduler, header lines, preparation and programs.
QString executionJobScript(const Document &document, const RemoteHost *remote = nullptr,
                           const ExecutionEnvironment &environment = executionEnvironment());
// How a job script is started on the computer that runs it, as the texts about a remote run show it:
// "sbatch job.sh" (slurm), "qsub job.sh" (pbs), else "nohup ./job.sh &" (shell).
QString jobSubmitCommand(const QString &scheduler);
// What "3 実行設定" shows instead of a local command: where the job goes and its job.sh, with the
// destination, preparation lines and header of the environment.
QString remoteJobPreview(const Document &document, const ExecutionEnvironment &environment);
} // namespace mf
