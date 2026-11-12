// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace mf {
// A computer that runs calculations over SSH ("リモート実行先"). Destinations are kept in Preferences
// only (execution_environment.h reads and saves them): a project names one ("remote" of the execution
// settings) but never says where it is, how to reach it or what runs there. Keys and passwords stay
// with ssh (ssh-agent, ~/.ssh/config).
struct RemoteHost {
    QString name;                          // As the project names it.
    QString host;                          // For ssh: "user@login.example.org" or a Host of ~/.ssh/config.
    int port = 0;                          // 0: as ssh is configured (else 22).
    QString directory = "~/molforma-jobs"; // Where the job folders go on that computer.
    QString scheduler = "shell";           // shell (in the background), slurm (sbatch) or pbs (qsub).
    // NWChem and the MPI launcher there; empty: as "3 実行設定" names them.
    QString executable, launcher;
    QString header;    // Lines added to the scheduler's header ("#SBATCH --account=…").
    QString prelude;   // Lines run before NWChem ("module load nwchem").
    int interval = 30; // Seconds between looks at a running job.
    QJsonObject json() const;
    static RemoteHost fromJson(const QJsonObject &object);
    bool operator==(const RemoteHost &other) const {
        return json() == other.json();
    }
};
QStringList remoteSchedulers(); // shell, slurm, pbs
QString remoteSchedulerName(const QString &scheduler);
QStringList remoteHostErrors(const RemoteHost &host);
// "user@host:~/molforma-jobs（Slurm）".
QString remoteHostSummary(const RemoteHost &host);
// ssh and scp of this computer, found in PATH (empty when missing).
QString remoteProgram(const QString &name);
} // namespace mf
