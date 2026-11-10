// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_execution.h"
#include <functional>

namespace mf {
struct QueuedCalculation {
    QString id, name, base, error;
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
    QString currentId;
    bool isPaused = true, stopOnError = true, scheduled = false, closed = false;
    int index(const QString &id) const;
    void schedule();
    void advance();
};
} // namespace mf
