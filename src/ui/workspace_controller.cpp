#include "ui/workspace_controller.h"
#include "nwchem/history.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QFileInfo>
#include <QStatusBar>
namespace mf {
WorkspaceController::WorkspaceController(MainWindow &w, ExecutionPanel &e, HistoryPanel &h, ResultsPanel &r)
    : QObject(&w), window(w), execution(e), history(h), results(r) {
    connect(&execution, &ExecutionPanel::bundleChanged, &history, &HistoryPanel::notifyJob);
    connect(&history, &HistoryPanel::reuseRequested, this, &WorkspaceController::reuse);
    connect(&history, &HistoryPanel::resultsRequested, this, &WorkspaceController::openResults);
    connect(&history, &HistoryPanel::executionCommitRequested, &execution,
            &ExecutionPanel::commitPendingEdits);
    connect(&history, &HistoryPanel::stopRequested, &execution, &ExecutionPanel::confirmStop);
    connect(&history, &HistoryPanel::runPanelRequested, this, [this] { execution.showRunPanel(); });
    connect(&execution, &ExecutionPanel::historyRequested, this, [this](const QString &directory) {
        window.showPanel(&history);
        if (!directory.isEmpty())
            history.selectDirectory(directory);
    });
    connect(&execution, &ExecutionPanel::resultsRequested, this, &WorkspaceController::openResults);
    connect(&execution, &ExecutionPanel::runFinished, this, &WorkspaceController::runFinished);
    connect(&results, &ResultsPanel::loaded, this, &WorkspaceController::resultLoaded);
}
void WorkspaceController::reuse(const QString &directory) {
    window.checkMcpPath(directory);
    if (window.hasCandidate() || window.chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に再利用してください。"));
    const auto next = historyDocument(directory);
    if (!window.replaceUnsavedDocument(next))
        return;
    window.showSetting(1);
    window.statusBar()->showMessage(
        tr("履歴から新しい未保存プロジェクトを作成しました。実行前に条件・参照ファイルを確認してください。"),
        15000);
}
void WorkspaceController::openResults(const QString &directory) {
    window.checkMcpPath(directory);
    if (resultFiles(directory).isEmpty())
        return;
    window.showPanel(&results);
    results.openDirectory(directory);
}
void WorkspaceController::runFinished(const QString &directory, bool open) {
    summaryDirectory.clear();
    if (open && !results.loading() && !resultFiles(directory).isEmpty()) {
        summaryDirectory = directory;
        results.openDirectory(directory);
    }
}
void WorkspaceController::resultLoaded() {
    if (summaryDirectory.isEmpty() ||
        QFileInfo(results.result().source).absolutePath() != QFileInfo(summaryDirectory).absoluteFilePath())
        return;
    execution.setResultSummary(summaryDirectory, results.briefSummary());
    summaryDirectory.clear();
}
} // namespace mf
