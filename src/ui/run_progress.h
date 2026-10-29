// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/run_state.h"
#include "results/results.h"
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace mf {
// "18:31", or "1:02:03" past an hour.
QString elapsedClock(qint64 seconds);
// A path with the home folder written as "~", as the panels show job folders.
QString displayPath(const QString &path);

// The tasks of a job with their state, and the four DRIVER criteria of a running optimization against
// their thresholds, read from the job folder's input.nw and, as it grows, its output.out. "3 実行設定"
// and "履歴" show a run with it. The rows are named <name>Stage<i>, the criteria <name>Criteria and
// <name>Criterion<i>.
class RunProgress : public QWidget {
  public:
    // columns: the criteria in one row (4) or in two (2).
    RunProgress(const QString &name, int columns, QWidget *parent = nullptr);
    // Reads only what the log gained since the last call for the same folder.
    void showRun(const QString &directory, RunState state);
    void clear();

  private:
    QString name;
    QVBoxLayout *stageList;
    QWidget *criteria;
    QList<QLabel *> criterionLabels;
    NwchemProgress progress;
    QString directory;
    qint64 offset = 0;
    QByteArray rest;
    QStringList tasks;
    int maxSteps = 0;
    void read(const QString &path);
    void setStages(const QStringList &stages);
};
} // namespace mf
