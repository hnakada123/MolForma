// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/nwchem.h"
#include "nwchem/run_state.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace mf {
struct RemoteHost;
// Expands "~" and "~/" to the home directory and resolves relative paths against base.
// Every execution path goes through this, and the MCP access policy checks the same result.
QString absolutePath(QString path, const QString &base);
// A UNC path (\\server\share or //server/share): on Windows, merely looking at one connects to that
// server and offers it the user's credentials.
bool networkPath(const QString &path);
// The network location the document's settings name where Preferences do not (empty if none):
// a project from elsewhere must not make MolForma look at it before the user chose it.
QString untrustedNetworkPath(const Document &document);
QJsonObject executionDefaults();
// The whole-number settings and their ranges, for the checks and the controls of "3 実行設定" and
// Preferences.
struct ExecutionRange {
    QString key;
    int minimum = 0, maximum = 0;
};
const QVector<ExecutionRange> &executionRanges();
ExecutionRange executionRange(const QString &key);
// Defaults the user saved in Preferences (QSettings "execution/defaults") for new documents.
// Only machine-wide keys are kept; per-project ones such as reference_directory are not.
QStringList executionPreferenceKeys();
QJsonObject executionPreferences();
void saveExecutionPreferences(const QJsonObject &settings);
// Writes the saved preferences into a new document (only values that differ from the defaults).
void applyExecutionPreferences(Document &document);
QJsonObject executionSettings(const Document &document);
void setExecutionSetting(Document &document, const QString &key, const QJsonValue &value);
// One item of the local checks before a run: the program, folder or input checked, what was
// found ("/usr/bin/nwchem"), and the error it stands for in ExecutionValidation::errors, if any.
struct ExecutionCheck {
    QString label, detail, error;
    bool ok = true;
};
struct ExecutionValidation : ValidationReport {
    QString executable, launcher, referenceDirectory;
    QString remote; // The destination of a remote run, in words (remoteHostSummary); empty for this PC.
    QMap<QString, QString> environment; // Only explicit overrides, never the inherited environment.
    QVector<ExecutionCheck> checks;     // With checkLocal: programs, folders and the NWChem input.
};
ExecutionValidation validateExecution(const Document &document, const QString &projectDirectory,
                                      bool checkLocal = true);
struct ExecutionPlan {
    QString directory, program, input;
    NwchemInputOutline inputOutline;
    QStringList arguments;
    QMap<QString, QString> environment;
    QJsonObject manifest;
    int timeoutSeconds = 0;
};
ExecutionPlan executionPlan(const Document &document, const QString &projectDirectory,
                            const QString &jobDirectory, bool checkLocal = true);
// Side-effect-free plan with a placeholder job directory; never saves or launches anything.
ExecutionPlan executionPreviewPlan(const Document &document, const QString &projectDirectory,
                                   bool checkLocal = false);
QString executionCommand(const ExecutionPlan &plan);
// The script "ジョブとして保存" puts beside the input (job.sh) for running the job on another computer:
// a plain shell script, or one for sbatch (Slurm) or qsub (PBS) with the header of its template
// (jobHeaderTemplate), by the setting "job_script"; empty for "none". It starts NWChem as "3 実行設定"
// names it, in the folder of the script, and leaves NWChem's exit code in the file exit_code. With a
// remote destination, the script of a run there: its scheduler, header lines, preparation and programs.
QString executionJobScript(const Document &document, const RemoteHost *remote = nullptr);
// Lines the user wants in every job script before NWChem starts ("module load nwchem"): kept in
// Preferences only, never in a project, since they are run as they are.
QString executionJobPrelude();
void saveExecutionJobPrelude(const QString &lines);
ExecutionPlan saveExecutionBundle(const Document &document, const QString &projectDirectory,
                                  bool forRun = false);
// Where a run's process lives, recorded in run.json when it starts: host, boot ID and, on Linux,
// the process start time, so a reused PID is not taken for the run.
QJsonObject executionProcessIdentity(qint64 pid);
// For a record left as starting/running/stopping: true when its process is known to have ended
// (recorded on this host, and the PID is gone or reused, or the system restarted since).
bool executionProcessGone(const QJsonObject &manifest);
// The supervisor mode of the executable ("--run-job <job folder>"): runs the job that folder was just
// prepared for, records its states in run.json and stops it when stop.request appears in the folder
// (or on SIGTERM), so that the calculation goes on when the application that started it ends.
// Returns -1 when the arguments ask for something else.
int runJobMain(int argc, char **argv);
// The program that supervises runs: this executable, or the AppImage it was started from (which
// stays mounted for as long as one of its processes runs).
QString runSupervisorProgram();
// Asks the supervisor of a running job to stop it. False when the job has no supervisor on this
// computer (then only the application that runs it can stop it).
bool requestRunStop(const QString &jobDirectory);
// Whether a record left as starting/running/stopping still has its supervisor.
bool runSupervised(const QJsonObject &manifest);
// For such a record: true when the run is known to have ended, its supervisor (if it had one) and
// NWChem alike (see executionProcessGone).
bool executionRunGone(const QJsonObject &manifest);
// One immutable calculation snapshot per run. Results never modify the editor document.
class NwchemRun : public QObject {
    Q_OBJECT
  public:
    explicit NwchemRun(QObject *parent = nullptr);
    ~NwchemRun() override;
    // With a program (this executable), runs go to a supervisor of their own (runJobMain) and are
    // followed through run.json; without one, NWChem is a child process that ends with this object.
    // Runs on a remote destination always need one.
    void setSupervisor(const QString &program) {
        supervisor = program;
    }
    bool supervised() const {
        return !supervisor.isEmpty();
    }
    QString supervisorProgram() const {
        return supervisor;
    }
    void start(const Document &document, const QString &projectDirectory);
    // Follows a supervised job that another session started, if it is still running: true then. A remote
    // run whose supervisor has ended gets a new one (resumeRemoteRun) when this object has a program.
    bool attach(const QString &jobDirectory);
    void stop();
    // Stops the run and waits for its end.
    void shutdown();
    // Leaves a supervised run to its supervisor and forgets it (the application ends).
    void release();
    bool active() const {
        return runStateActive(status);
    }
    const ExecutionPlan &plan() const {
        return current;
    }
    RunState state() const {
        return status;
    }
    QString message() const {
        return detail;
    }
    qint64 elapsedSeconds() const;
  signals:
    void changed();

  private:
    friend class NwchemQueue;
    friend int runJobMain(int argc, char **argv);
    void startPrepared(const ExecutionPlan &plan);
    QProcess process;
    QTimer timer, killTimer;
    QElapsedTimer elapsed;
    ExecutionPlan current;
    RunState status = RunState::Idle;
    QString detail, supervisor;
    bool timedOut = false, watching = false; // watching: the run is its supervisor's.
    qint64 groupId = 0, duration = 0, supervisorPid = 0;
    int unreadable = 0; // Polls in a row that could not read run.json.
    int resumed = 0;    // Supervisors started again for a remote run this object follows.
    void launch();
    void poll();
    void signalProcess(bool force);
    void finish(int exitCode, QProcess::ExitStatus exitStatus, const QString &error = {});
    void record();
};
} // namespace mf
