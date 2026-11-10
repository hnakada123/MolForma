// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_execution.h"
#include <functional>

namespace mf {
struct QueuedCalculation {
    QString id, name, base, error;
    QString record; // The digest of the job's run.json as it was registered.
    Document document;
    ExecutionPlan plan;
    RunState state = RunState::Prepared;
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
    QString enqueue(const Document &document, const QString &base, std::function<void()> checkAccess = {});
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

  private:
    NwchemRun &run;
    QVector<QueuedCalculation> items;
    QString currentId, store;
    void save() const;
    bool isPaused = true, stopOnError = true, scheduled = false, closed = false;
    int index(const QString &id) const;
    void schedule();
    void advance();
};
} // namespace mf
