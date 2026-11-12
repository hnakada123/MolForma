// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/run_session.h"
#include "results/results.h"
#include "ui/editor_context.h"
#include <QStringDecoder>
#include <QWidget>
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
class ChoiceButtons;
class ExecutionForm;
class RunProgress;
class SettingsCard;
class StatusCard;
// "3 実行設定": the settings of a run at the left (programs and folders, resources, environment),
// and at the right the checks before a run and the command. The run itself, its progress (tasks,
// DRIVER steps against their thresholds) and its log are in the run panel below the tabs.
// The run and the queue are the session's (RunSession, which the window owns and outlives this tab's
// view of them): the panel shows them, and asks for calculations once the user's gate has let them pass
// (no candidate, trusted programs, MCP's folders).
class ExecutionPanel : public QWidget {
    Q_OBJECT
  public:
    ExecutionPanel(EditorContext &editorContext, UiServices &uiContext, WorkspaceActions &workspace,
                   ExecutionAccess &executionAccess, RunSession &runs, QWidget *parent = nullptr);
    void refresh();
    void stopEditing();
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
    // What the window shows of the run outside this panel (its header and menus): whether a run is
    // active, the text of the run pill ("実行中 0:42") and whether the run can be stopped now.
    void runDisplayChanged(bool active, QString text, bool stoppable);
    // The run panel was opened or closed.
    void runPanelToggled(bool shown);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Asks the user whether to run with execution settings they have not entered or confirmed.
    bool confirmExecution();
    // The gate of a calculation: MCP's folders for its request, and programs the user has confirmed.
    // False when the user gave up.
    bool authorizeCalculation();
    EditorContext &editorContext;
    UiServices &uiContext;
    WorkspaceActions &workspace;
    ExecutionAccess &executionAccess;
    RunSession &session;
    NwchemRun &run;
    NwchemQueue &queue;
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
