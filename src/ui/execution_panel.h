#pragma once
#include "nwchem/nwchem_execution.h"
#include <QMap>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QFormLayout;
class QSystemTrayIcon;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
namespace mf {
class MainWindow;
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
    void saveBundle();
    const NwchemRun &currentRun() const {
        return run;
    }
    void stopCalculation() {
        run.stop();
    }
  signals:
    void bundleChanged(QString directory);

  private:
    friend class McpGui;
    MainWindow *window;
    NwchemRun run;
    bool busy = false, environmentDirty = false;
    QMap<QString, QWidget *> fields;
    QWidget *form;
    QTableWidget *environment;
    QComboBox *mode, *logChannel;
    QSpinBox *memory;
    QLabel *resources, *state, *location;
    QPlainTextEdit *diagnostics, *command, *log;
    QPushButton *start, *stop, *prepare, *undo, *redo, *openFolder;
    QPushButton *viewResults;
    QCheckBox *openResults; // Load the job folder into the result viewer when a run finishes.
    QSystemTrayIcon *tray = nullptr;
    ExecutionPlan saved;
    QString displayedLog, previousRunState;
    QString baseDirectory() const;
    void change(const QString &key, const QJsonValue &value);
    void editEnvironment();
    void updateRun();
    void runEnded();
    void pathField(QFormLayout *layout, const QString &label, const QString &key, bool directory);
    void integer(QFormLayout *layout, const QString &label, const QString &key, int lo, int hi);
};
} // namespace mf
