// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
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
// DRIVER steps against their thresholds) and its log are in the run panel over the tabs.
class ExecutionPanel : public QWidget {
    Q_OBJECT
  public:
    explicit ExecutionPanel(MainWindow *owner);
    void refresh();
    void stopEditing();
    void shutdown();
    void commitPendingEdits();
    void showField(const QString &key); // Focuses this panel's own setting control.
    bool running() const {
        return run.active();
    }
    void startCalculation();
    // "計算を実行" (header, menu, F5): starts when "3 実行設定" allows it, otherwise shows that tab
    // and why.
    void startFromMenu();
    // The run panel over the tabs (state, progress and log of the current or last run).
    void showRunPanel(bool show = true);
    bool runPanelShown() const;
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
    void resultsRequested(QString directory);
    void runFinished(QString directory, bool openResults);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Asks the user whether to run with execution settings they have not entered or confirmed.
    bool confirmExecution();
    MainWindow *window;
    NwchemRun run;
    bool busy = false, startable = false;
    QWidget *form, *cpuMeter;
    ExecutionForm *settingsForm; // Programs and folders, resources and environment.
    QComboBox *logChannel;
    QList<ChoiceButtons *> choices;
    QSpinBox *memory;
    QLabel *resources, *memoryTotal, *state, *location, *statePill, *timing, *diagnostics, *context,
        *startHint, *logName;
    QVBoxLayout *checkList;
    RunProgress *progress;
    QPlainTextEdit *command, *log;
    QPushButton *prepare, *openFolder, *viewResults, *showRunLink, *stopRun;
    QFrame *runPanel;
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
