// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
namespace mf {
class MainWindow;
class ExecutionPanel;
class HistoryPanel;
class ResultsPanel;
// Coordinates workflows spanning panels. Each panel owns its display and emits requests;
// project replacement, navigation and loading a completed run are wired here.
class WorkspaceController : public QObject {
  public:
    WorkspaceController(MainWindow &window, ExecutionPanel &execution, HistoryPanel &history,
                        ResultsPanel &results);

  private:
    MainWindow &window;
    ExecutionPanel &execution;
    HistoryPanel &history;
    ResultsPanel &results;
    QString summaryDirectory;
    void reuse(const QString &directory);
    void openResults(const QString &directory);
    void runFinished(const QString &directory, bool open);
    void resultLoaded();
};
} // namespace mf
