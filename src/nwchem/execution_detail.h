// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Internal to src/nwchem: what the files about running a calculation share with one another
// (execution_settings, execution_plan, job_script, run_process, run_supervisor, nwchem_run and the
// remote_* files), each with one definition: here, or in the file named beside it.
namespace mf::execution_detail {
// A text as one word of a POSIX shell.
inline QString quote(QString s) {
    s.replace('\'', "'\\''");
    return "'" + s + "'";
}
// Whether a text has control characters. With lines, a text of several lines: tabs and line feeds are
// none.
inline bool controls(const QString &s, bool lines = false) {
    return s.contains(QRegularExpression(lines ? "[\\x00-\\x08\\x0b-\\x1f\\x7f]" : "[\\x00-\\x1f\\x7f]"));
}
// execution_settings.cpp: the variables of the resource settings (threads, stack), which the rows of
// "environment" must not name and which a job script sets for its job.
extern const QStringList managedEnvironment;
bool writableDirectory(const QString &path, bool mayCreate);
// The folder of the jobs ("job_root") of execution settings.
QString executionRoot(const QJsonObject &s, const QString &base);
// job_script.cpp: how job.sh and the command shown for a run send NWChem's output and errors to their
// files.
QString outputRedirection();
// run_process.cpp: the file that asks the supervisor of a job to stop it.
QString stopRequest(const QString &directory);
// run_process.cpp: ends the process group of a run (Unix), or the whole tree of its first process
// (Windows).
void signalGroup(qint64 group, bool force);
} // namespace mf::execution_detail
