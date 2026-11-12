// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/workspace_controller.h"
#include "editing/coordinates.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/result_files.h"
#include "nwchem/run_record.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QDir>
#include <QFileInfo>
#include <QStatusBar>
namespace mf {
WorkspaceController::WorkspaceController(MainWindow &w, ExecutionPanel &e, HistoryPanel &h, ResultsPanel &r)
    : QObject(&w), window(w), execution(e), history(h), results(r) {
    connect(&execution, &ExecutionPanel::bundleChanged, &history, &HistoryPanel::notifyJob);
    connect(&history, &HistoryPanel::reuseRequested, this, &WorkspaceController::reuse);
    connect(&history, &HistoryPanel::restartRequested, this, &WorkspaceController::restart);
    connect(&execution, &ExecutionPanel::restartRequested, this, &WorkspaceController::restart);
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
    connect(&results, &ResultsPanel::recalculationRequested, this,
            &WorkspaceController::prepareRecalculation);
    connect(&results, &ResultsPanel::loaded, this, &WorkspaceController::resultLoaded);
    connect(&window, &MainWindow::panelChanged, this, [this] {
        if (window.currentPanel() == &results)
            execution.resultsShown();
    });
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
void WorkspaceController::restart(const QString &directory, const QString &mode) {
    window.checkMcpPath(directory);
    for (const auto *file : {JobFile::record, JobFile::project, JobFile::input, JobFile::output})
        window.checkMcpPath(jobFile(directory, file));
    if (window.hasCandidate() || window.chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に準備してください。"));
    const auto next = mode == "orbitals" ? orbitalReuseDocument(directory) : restartDocument(directory, mode);
    if (!window.replaceUnsavedDocument(next))
        return;
    window.showSetting(1);
    window.statusBar()->showMessage(
        mode == "orbitals" ? tr("分子軌道を引き継ぐ新しい未保存プロジェクトを作成しました。同じ原子順・基底で"
                                "実行してください。")
        : mode == "continue"
            ? tr("続きから実行する新しい未保存プロジェクトを作成しました。条件を確認して「計算を実行」を押し"
                 "て"
                 "ください。")
            : tr("Hessianを引き継ぐ新しい未保存プロジェクトを作成しました。条件を確認して「計算を実行」を押し"
                 "てください。"),
        15000);
}
void WorkspaceController::openResults(const QString &directory) {
    window.checkMcpPath(directory);
    if (nwchemResultFiles(directory).isEmpty())
        return;
    window.showPanel(&results);
    results.openDirectory(directory);
    execution.resultsShown();
}
void WorkspaceController::runFinished(const QString &directory, bool open) {
    summaryDirectory.clear();
    if (open && !results.loading() && !nwchemResultFiles(directory).isEmpty()) {
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
    if (window.currentPanel() == &results)
        execution.resultsShown();
}
void WorkspaceController::prepareRecalculation(const QString &directory, const Document &geometry,
                                               const QString &task) {
    if (window.hasCandidate() || window.chemistryRunning() || execution.running())
        fail(tr("候補・分子処理・実行中の計算を終了してから再計算を準備してください。"));
    window.checkMcpPath(directory);
    for (const auto *file : {JobFile::record, JobFile::project, JobFile::input})
        window.checkMcpPath(jobFile(directory, file));
    auto original = historyDocument(directory);
    // Another calculation from a point of the path: the task, under a name of its own where that fits.
    if (!task.isEmpty())
        deriveNwchemCalculation(original, task, "_ts", true);
    auto candidate = original;
    applyResultGeometry(candidate, geometry); // Check atom order and fixed atoms before replacing anything.
    if (!window.replaceUnsavedDocument(original))
        return;
    window.edit(tr("計算結果の構造を適用"), [&](Document &d) { applyResultGeometry(d, geometry); }, true);
    window.showPanel(0);
    window.statusBar()->showMessage(
        task.isEmpty()
            ? tr("保存した条件で再計算を準備しました。構造と条件を確認して新しいジョブを実行してください。")
            : tr("選んだ点から遷移状態探索を準備しました。構造と条件を確認して新しいジョブを実行してください"
                 "。"),
        15000);
}
} // namespace mf
