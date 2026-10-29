#pragma once
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
#include <QMap>
#include <QStringDecoder>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QSystemTrayIcon;
class QToolButton;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;
namespace mf {
class MainWindow;
class ChoiceButtons;
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
    bool running() const {
        return run.active();
    }
    void startCalculation();
    // "計算を実行" (header, menu, F5): starts when "3 実行設定" allows it, otherwise shows that tab
    // and why.
    void startFromMenu();
    bool canStart() const {
        return startable;
    }
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
  signals:
    void bundleChanged(QString directory);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    MainWindow *window;
    NwchemRun run;
    bool busy = false, environmentDirty = false, startable = false;
    QMap<QString, QWidget *> fields;
    QMap<QString, QLabel *> found; // Where the executable and the launcher were found.
    QWidget *form, *cpuMeter;
    QTableWidget *environment;
    QComboBox *mode, *logChannel;
    QList<ChoiceButtons *> choices;
    QSpinBox *memory;
    QLabel *resources, *memoryTotal, *environmentEmpty, *state, *location, *statePill, *timing, *diagnostics,
        *context, *startHint, *logName;
    QVBoxLayout *checkList;
    RunProgress *progress;
    QPlainTextEdit *command, *log;
    QPushButton *prepare, *openFolder, *viewResults, *removeEnvironment, *showRunLink, *stopRun;
    QFrame *runPanel;
    QFrame *toolbar;        // The calculation, "終了したら結果ビューアに読み込む" and "ジョブとして保存".
    QCheckBox *openResults; // Load the job folder into the result viewer when a run finishes.
    QCheckBox *follow;      // Keep the end of the log in view.
    SettingsCard *runCard, *commandCard;
    StatusCard *checkCard;
    QLabel *runningNote;
    QSystemTrayIcon *tray = nullptr;
    ExecutionPlan saved;
    QString previousRunState;
    // The log view appends what the file gained since the last look, so a selection stays.
    QString logPath;
    qint64 logOffset = -1;
    QStringDecoder logDecoder{QStringDecoder::Utf8};
    QToolButton *runStatus; // Permanent status-bar entry for the current or last run.
    // The finished run whose log is loading into the result viewer, and that log's outcome.
    QString summaryDirectory, resultSummary;
    void showResultSummary();
    void appendLog();
    void change(const QString &key, const QJsonValue &value);
    void editEnvironment();
    void updateRun();
    void runEnded();
    void buildRunPanel();
    void placeRunPanel();
    void pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory);
    QWidget *integer(const QString &label, const QString &key, int lo, int hi, const QString &unit = {});
};
} // namespace mf
