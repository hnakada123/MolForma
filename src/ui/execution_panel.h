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
// "5 実行設定": the settings of a run at the left (programs and folders, resources, environment),
// and at the right, top to bottom, the checks before a run, the command, the run itself, its
// progress (tasks, DRIVER steps against their thresholds) and its log.
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
    // "計算 → 計算を実行": starts when "5 実行設定" allows it, otherwise shows that tab and why.
    void startFromMenu();
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
  signals:
    void bundleChanged(QString directory);

  private:
    friend class McpGui;
    MainWindow *window;
    NwchemRun run;
    bool busy = false, environmentDirty = false;
    QMap<QString, QWidget *> fields;
    QMap<QString, QLabel *> found; // Where the executable and the launcher were found.
    QWidget *form, *cpuMeter;
    QTableWidget *environment;
    QComboBox *mode, *logChannel;
    QList<ChoiceButtons *> choices;
    QSpinBox *memory;
    QLabel *resources, *memoryTotal, *environmentEmpty, *state, *location, *statePill, *timing, *diagnostics;
    QVBoxLayout *checkList;
    RunProgress *progress;
    QPlainTextEdit *command, *log;
    QPushButton *start, *stop, *prepare, *openFolder, *viewResults, *removeEnvironment;
    QCheckBox *openResults; // Load the job folder into the result viewer when a run finishes.
    SettingsCard *runCard, *checkCard, *commandCard;
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
    QString baseDirectory() const;
    void change(const QString &key, const QJsonValue &value);
    void editEnvironment();
    void updateRun();
    void runEnded();
    void pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory);
    QWidget *integer(const QString &label, const QString &key, int lo, int hi, const QString &unit = {});
};
} // namespace mf
