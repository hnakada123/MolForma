// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_run.h"
#include <functional>

namespace mf {
// Where a queued calculation takes its structure from: the job of an earlier entry of the queue, once
// that has finished. structure: "final" (its last structure), "highest" (the highest point of a scan,
// IRC or NEB path, the ends of an NEB aside), "irc_forward" / "irc_backward" (the last point of an IRC
// direction).
struct QueueChain {
    QString from, structure;
    QString directory; // The job folder of that entry, once known (kept for a later session).
    bool active() const {
        return !from.isEmpty();
    }
};
QStringList queueChainStructures();
QString queueChainStructureName(const QString &structure); // In words ("最終構造").
// The document with its atoms moved to a structure of a job (its output.out): the same atoms in the same
// order, fixed atoms where they are. Fails when the job has no such structure.
Document chainedDocument(const Document &document, const QString &job, const QString &structure);
struct QueuedCalculation {
    QString id, name, base, error;
    QString record; // The digest of the job's run.json as it was registered.
    // The registered document. A chained calculation keeps it until its turn, when it takes the structure
    // of the earlier job and only then saves its bundle (plan.directory is empty until then).
    Document document;
    ExecutionPlan plan;
    RunState state = RunState::Prepared;
    QueueChain chain;
    std::function<void()> checkAccess;
};
// A session-local sequential queue over the same runner used for immediate calculations.
// Each entry owns its registration-time snapshot and saved bundle. It never edits the document.
class NwchemQueue : public QObject {
    Q_OBJECT
  public:
    explicit NwchemQueue(NwchemRun &runner, QObject *parent = nullptr);
    // Keeps the waiting and running entries in a file from now on, so that another session can take
    // them up (restore). Without one the queue lasts as long as this object.
    void setStore(const QString &path);
    // Takes up the entries a session left in the store: those still waiting, with the documents of
    // their job folders, and the one that is still running (the runner follows it). The queue stays
    // paused. Entries whose job has changed since are dropped. Returns the number taken up.
    int restore();
    // With a chain, the entry takes its structure from an earlier entry when its turn comes; its bundle is
    // saved then. Cancelling an entry also cancels the waiting entries that take their structure from it.
    // The entry is checked and its bundle saved with the environment (the Preferences as they are now,
    // unless the caller registers several with one snapshot); when its turn comes, it is checked again
    // with the Preferences of that moment.
    QString enqueue(const Document &document, const QString &base, std::function<void()> checkAccess = {},
                    const QueueChain &chain = {},
                    const ExecutionEnvironment &environment = executionEnvironment());
    // Calculations that go on from an entry (from), with the conditions of `current` and only their tasks
    // and names changed: "current" (the document as it is), else the steps of a reaction path in this
    // order: "saddle" (transition state → frequencies, name + "_ts"), "irc" (both directions, "_irc"),
    // "ends" (an optimization of each end of the IRC, "_fwd" and "_bwd", with frequencies when
    // endFrequencies). The first takes `structure` of the entry, the others the final structure or an
    // end of the step before. All are checked before any is registered. Returns their IDs.
    QStringList enqueueChain(const Document &current, const QString &base, const QString &from,
                             const QString &structure, const QStringList &steps, bool endFrequencies = true,
                             const std::function<std::function<void()>(const Document &)> &access = {});
    void resume();
    void pause();                   // The active calculation continues.
    void cancel(const QString &id); // Waiting entries only; stop the runner for the active one.
    void move(const QString &id, int position);
    void setStopOnFailure(bool stop);
    void shutdown();
    bool paused() const {
        return isPaused;
    }
    bool stopOnFailure() const {
        return stopOnError;
    }
    int pending() const;
    const QVector<QueuedCalculation> &entries() const {
        return items;
    }
    QJsonObject json() const;
  signals:
    void changed();
    // A chained entry saved its bundle when its turn came.
    void bundleSaved(QString directory);

  private:
    NwchemRun &run;
    QVector<QueuedCalculation> items;
    QString currentId, store;
    void save() const;
    bool isPaused = true, stopOnError = true, scheduled = false, closed = false;
    int index(const QString &id) const;
    int find(const QString &id) const; // -1 when absent.
    void checkOrder() const;
    void schedule();
    void advance();
};
} // namespace mf
