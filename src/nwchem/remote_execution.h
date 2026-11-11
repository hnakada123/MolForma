// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <optional>

namespace mf {
// A computer that runs calculations over SSH ("リモート実行先"). Destinations are kept in Preferences
// only: a project names one ("remote" of the execution settings) but never says where it is, how to
// reach it or what runs there. Keys and passwords stay with ssh (ssh-agent, ~/.ssh/config).
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
// The destinations saved in Preferences (QSettings "execution/remotes").
QVector<RemoteHost> remoteHosts();
// Checks every destination (remoteHostErrors, names told apart) and saves them; fails otherwise.
void saveRemoteHosts(const QVector<RemoteHost> &hosts);
QStringList remoteHostErrors(const RemoteHost &host);
std::optional<RemoteHost> findRemoteHost(const QString &name);
// "user@host:~/molforma-jobs（Slurm）".
QString remoteHostSummary(const RemoteHost &host);
// ssh and scp of this computer, found in PATH (empty when missing).
QString remoteProgram(const QString &name);

// The header of a job script for a scheduler (slurm, pbs): a template of lines starting with "#" whose
// placeholders ({name}, {ranks}, …) take the values of the calculation. A line with a placeholder that
// has no value ({time} without a time limit, {queue} without a queue) is left out.
QStringList jobHeaderPlaceholders();
QString defaultJobHeader(const QString &scheduler);
// The template saved in Preferences, else the default.
QString jobHeaderTemplate(const QString &scheduler);
// Saves a template (the default when it is empty or the same); fails when jobHeaderErrors finds any.
void saveJobHeaderTemplate(const QString &scheduler, const QString &text);
QStringList jobHeaderErrors(const QString &text);
QStringList renderJobHeader(const QString &text, const QMap<QString, QString> &values);

// Runs on a remote destination. The record (run.json) of such a run has "remote": the destination as it
// was ("host"), the job folder there ("directory"), the scheduler's ID of the job ("job"), what was last
// seen ("state") and why the last look failed ("message", "detail").
bool remoteRun(const QJsonObject &manifest);
// One line on where a remote run is and how it stands, for the run panel and the history.
QString remoteRunText(const QJsonObject &manifest);
// The supervisor (runJobMain) of a remote run: copies the job there, submits it, follows it and copies
// the folder back when it ends. Started again for a job it left (resume), it goes on from the record.
int remoteJobMain(const QString &directory, QJsonObject manifest);
// Starts a supervisor for a remote run whose supervisor has ended (the computer restarted): it looks
// at the job again and copies its results back once it ends. False when the record is no such run
// (or is followed on another computer). program: this executable (runSupervisorProgram()).
bool resumeRemoteRun(const QString &directory, const QString &program, qint64 *pid = nullptr);
// What "3 実行設定" shows instead of a local command: where the job goes and its job.sh.
QString remoteJobPreview(const Document &document);

// "接続を確認" of a destination: connects once, runs its preparation lines and reports what was found
// there (the folder, the scheduler's commands, NWChem and the launcher).
class RemoteProbe : public QObject {
    Q_OBJECT
  public:
    explicit RemoteProbe(QObject *parent = nullptr);
    ~RemoteProbe() override;
    // executable and launcher: as "3 実行設定" names them when the destination does not.
    void start(const RemoteHost &host, const QString &executable = "nwchem",
               const QString &launcher = "mpirun");
    void cancel();
    bool active() const {
        return process.state() != QProcess::NotRunning;
    }
  signals:
    void finished(bool ok, QString report);

  private:
    QProcess process;
    RemoteHost probed;
    QString nonce, executable, launcher;
    int probes = 0;          // Probes started, so that a time limit ends only its own.
    bool cancelling = false; // A cancelled probe reports nothing.
    void done();
};
} // namespace mf
