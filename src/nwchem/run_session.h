// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_queue.h"
#include <QLockFile>
#include <functional>
#include <memory>

namespace mf {
// The calculations of one session: the run, the queue over it, the bundle saved last and what the session
// leaves for the next one (the running job and the waiting entries). No widgets, and no say in who may run
// what: the window and the headless MCP server each hold one, check a request at their own gate (candidates,
// trusted programs, MCP's folders) and then ask the session.
class RunSession : public QObject {
    Q_OBJECT
  public:
    // The check of a calculation's paths for the one who asked for it (MCP's allowed folders): fails to
    // refuse. The session applies it again when a queued calculation's turn comes, as its bundle is saved
    // or its run starts only then. Empty: nothing to check.
    using Check = std::function<void(const Document &, const QString &base)>;
    explicit RunSession(QObject *parent = nullptr);
    NwchemRun &run() {
        return runner;
    }
    const NwchemRun &run() const {
        return runner;
    }
    NwchemQueue &queue() {
        return pending;
    }
    const NwchemQueue &queue() const {
        return pending;
    }
    // The bundle saved last by saveBundle ("ジョブとして保存", MCP's save_bundle).
    const ExecutionPlan &saved() const {
        return bundle;
    }

    // Runs get a supervisor of their own (this executable, runSupervisorProgram) and go on when the session
    // ends. The session then remembers the running job for the next one (Preferences "execution/running").
    void supervise(const QString &program);
    // Keeps the waiting entries of the queue in this folder (queue.json) from now on, unless another
    // session holds them there (queue.lock): false then, and the queue lasts as long as this session.
    bool keepQueue(const QString &folder);
    // What an earlier session left, taken up by restore.
    struct Restored {
        int waiting = 0;        // Entries of the queue that wait (the queue stays paused).
        QString ended;          // The job that session left running and that has ended since; empty if none.
        bool following = false; // A run is active: the one the queue or that session left.
    };
    // Takes up the stored queue (withQueue, if this session keeps it) and follows the job the last session
    // left running, if it still runs. checkPath refuses a job this session may not look at (MCP's allowed
    // folders): it is left alone then, for a session that may.
    Restored restore(bool withQueue, const std::function<void(const QString &)> &checkPath = {});
    // Ends the session: stops the run, or with keepRunning leaves a supervised run to its supervisor. The
    // waiting entries stay where the queue keeps them.
    void shutdown(bool keepRunning = false);

    // Saves the input bundle of a calculation without running it.
    ExecutionPlan saveBundle(const Document &document, const QString &base);
    // Registers a calculation in the queue; with a chain it takes its structure from an earlier entry when
    // its turn comes (QueueChain). Returns its ID.
    QString enqueue(const Document &document, const QString &base, const Check &check = {},
                    const QueueChain &chain = {});
    // Calculations that go on from an entry of the queue (NwchemQueue::enqueueChain). Returns their IDs.
    QStringList enqueueChain(const Document &current, const QString &base, const QString &from,
                             const QString &structure, const QStringList &steps, bool endFrequencies = true,
                             const Check &check = {});
    // One optimization (with frequencies, if asked) per structure, with the conditions of `current`: the
    // same atoms in the same order, each under the name of the calculation + "_conf<n>". All are checked
    // before any is registered. Returns their job folders.
    QStringList enqueueGeometries(const Document &current, const QString &base,
                                  const QVector<Document> &geometries, bool frequencies,
                                  const Check &check = {});
  signals:
    // The bundle of a job was saved, or the state of the run in it changed: its folder.
    void bundleChanged(QString directory);

  private:
    NwchemRun runner;
    NwchemQueue pending;
    std::unique_ptr<QLockFile> queueLock; // One session at a time keeps the stored queue.
    ExecutionPlan bundle;
    RunState previous = RunState::Idle;
    // The check of a queued document at its turn.
    static std::function<void()> turn(const Check &check, const Document &document, const QString &base);
};
} // namespace mf
