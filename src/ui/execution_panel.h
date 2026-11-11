// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_queue.h"
#include "results/results.h"
#include <QLockFile>
#include <QStringDecoder>
#include <QWidget>
#include <memory>
class QCheckBox;
class QComboBox;
class QFrame;
class QSystemTrayIcon;
class QToolButton;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSplitter;
class QVBoxLayout;
namespace mf {
class MainWindow;
class ChoiceButtons;
class ExecutionForm;
class RunProgress;
class SettingsCard;
class StatusCard;
// "3 実行設定": the settings of a run at the left (programs and folders, resources, environment),
// and at the right the checks before a run and the command. The run itself, its progress (tasks,
// DRIVER steps against their thresholds) and its log are in the run panel below the tabs.
class ExecutionPanel : public QWidget {
    Q_OBJECT
  public:
    explicit ExecutionPanel(MainWindow *owner);
    void refresh();
    void stopEditing();
    // Ends the session: stops the run, or with keepRunning leaves a supervised run to go on
    // (the waiting queue entries are kept for the next session either way, where they are stored).
    void shutdown(bool keepRunning = false);
    // Interactive sessions: runs get a supervisor of their own (this executable) and go on when the
    // window closes, and the queue is kept in a file of the application's data folder.
    void superviseRuns(const QString &program);
    bool runsSupervised() const {
        return run.supervised();
    }
    // Takes up what an earlier session left running or waiting. Returns what to tell the user about it
    // (empty when there was nothing).
    QString restoreRuns(bool withQueue = true);
    void commitPendingEdits();
    void showField(const QString &key); // Focuses this panel's own setting control.
    bool running() const {
        return run.active();
    }
    void startCalculation();
    // With a chain, the calculation takes its structure from an earlier entry of the queue when its turn
    // comes (QueueChain).
    QString enqueueCalculation(const QueueChain &chain = {});
    // Calculations that go on from an entry of the queue (from: its ID; structure as QueueChain), with
    // the current conditions and only their tasks changed: "current" (as they are), else the steps of a
    // reaction path in this order: "saddle" (transition state → frequencies), "irc" (both directions),
    // "ends" (an optimization of each end of the IRC, with frequencies when endFrequencies). Returns the
    // IDs of the new entries.
    QStringList enqueueChain(const QString &from, const QString &structure, const QStringList &steps,
                             bool endFrequencies = true);
    void showChainDialog();
    QStringList enqueueGeometries(const QVector<Document> &geometries, bool frequencies);
    void showQueue();
    NwchemQueue &calculationQueue() {
        return queue;
    }
    // "計算を実行" (header, menu, F5): starts when "3 実行設定" allows it, otherwise shows that tab
    // and why.
    void startFromMenu();
    // The run panel below the tabs (state, progress and log of the current or last run). `split`
    // holds the tabs, which give up the height of the panel while it is open.
    void dockRunPanel(QSplitter *split);
    void showRunPanel(bool show = true);
    bool runPanelShown() const;
    // Gives the tabs their place back where the run panel fills it (a tab is asked for).
    void showWorkspace();
    // The result viewer is in view: closes the panel of a run that ended well, left open since then.
    void resultsShown();
    // Asks before stopping the running NWChem (the stop buttons and "計算 → 停止").
    void confirmStop();
    void saveBundle();
    const NwchemRun &currentRun() const {
        return run;
    }
    void stopCalculation() {
        run.stop();
    }
    void applyChromeStyle();
    QString baseDirectory() const;
    QString savedDirectory() const {
        return saved.directory;
    }
    void setResultSummary(const QString &directory, const QString &summary);
  signals:
    void bundleChanged(QString directory);
    void historyRequested(QString directory);
    // Going on from the run the panel shows, which did not end ("continue" of NwchemRestart).
    void restartRequested(QString directory, QString mode);
    void resultsRequested(QString directory);
    void runFinished(QString directory, bool openResults);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Asks the user whether to run with execution settings they have not entered or confirmed.
    bool confirmExecution();
    bool authorizeCalculation();
    MainWindow *window;
    NwchemRun run;
    NwchemQueue queue;
    std::unique_ptr<QLockFile> queueLock; // One session at a time keeps the stored queue.
    bool busy = false, startable = false;
    QWidget *form, *cpuMeter;
    ExecutionForm *settingsForm; // Programs and folders, resources and environment.
    QComboBox *logChannel;
    QList<ChoiceButtons *> choices;
    QSpinBox *memory;
    QLabel *resources, *memoryTotal, *state, *location, *statePill, *timing, *diagnostics, *context,
        *startHint, *logName, *remoteLine;
    QVBoxLayout *checkList;
    RunProgress *progress;
    QPlainTextEdit *command, *log;
    QPushButton *prepare, *openFolder, *viewResults, *showRunLink, *stopRun;
    QPushButton *enqueue, *queueButton;
    QPushButton *diagnosisSetting, *continueRun;
    QString continueKey; // The run and state continueRun was looked up for, and what it found.
    bool continuable = false;
    QFrame *runPanel;
    QSplitter *dock = nullptr;
    QToolButton *maximize;
    int panelHeight = 0;     // As dragged; 0 follows the window.
    bool panelFull = false;  // Asked to fill the place of the tabs.
    bool panelTight = false; // The window is too low for the tabs and the panel together.
    bool closeOnResults = false;
    QFrame *toolbar;        // The calculation, "終了したら結果ビューアに読み込む" and "ジョブとして保存".
    QCheckBox *openResults; // Load the job folder into the result viewer when a run finishes.
    QCheckBox *follow;      // Keep the end of the log in view.
    SettingsCard *runCard, *commandCard;
    StatusCard *checkCard;
    QLabel *runningNote;
    QSystemTrayIcon *tray = nullptr;
    ExecutionPlan saved;
    RunState previousRunState = RunState::Idle;
    // The log view appends what the file gained since the last look, so a selection stays.
    QString logPath;
    qint64 logOffset = -1;
    QStringDecoder logDecoder{QStringDecoder::Utf8};
    QToolButton *runStatus; // Permanent status-bar entry for the current or last run.
    // The finished run whose log is loading into the result viewer, and that log's outcome.
    QString resultSummary;
    void appendLog();
    void change(const QString &key, const QJsonValue &value);
    void updateRun();
    void runEnded();
    void buildRunPanel();
    void placeRunPanel();
};
} // namespace mf
