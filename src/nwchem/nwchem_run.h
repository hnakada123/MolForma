// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/execution_plan.h"
#include "nwchem/run_state.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace mf {
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
