// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
namespace mf {
class Document;
class MainWindow;
class CalculationPanel;
class ExecutionPanel;
class HistoryPanel;
class ResultsPanel;
// Coordinates workflows spanning panels. Each panel owns its display and emits requests;
// project replacement, navigation, loading a completed run and what one panel takes from another
// (the structure the result viewer shows, as the end of an NEB path) are wired here.
class WorkspaceController : public QObject {
  public:
    WorkspaceController(MainWindow &window, CalculationPanel &calculation, ExecutionPanel &execution,
                        HistoryPanel &history, ResultsPanel &results);

  private:
    MainWindow &window;
    CalculationPanel &calculation;
    ExecutionPanel &execution;
    HistoryPanel &history;
    ResultsPanel &results;
    QString summaryDirectory;
    void reuse(const QString &directory);
    void restart(const QString &directory, const QString &mode);
    void openResults(const QString &directory);
    void runFinished(const QString &directory, bool open);
    void resultLoaded();
    // "結果ビューアの構造" of the NEB end: the structure the result viewer shows; fails while it
    // shows none.
    void nebEndFromResult();
    void prepareRecalculation(const QString &directory, const Document &geometry, const QString &task);
};
} // namespace mf
