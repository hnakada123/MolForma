#pragma once
#include "nwchem_execution.h"
#include <QMap>
#include <QWidget>
class QComboBox;
class QFormLayout;
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

  private:
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
    ExecutionPlan saved;
    QString displayedLog, previousRunState;
    QString baseDirectory() const;
    void change(const QString &key, const QJsonValue &value);
    void editEnvironment();
    void updateRun();
    void pathField(QFormLayout *layout, const QString &label, const QString &key, bool directory);
    void integer(QFormLayout *layout, const QString &label, const QString &key, int lo, int hi);
};
} // namespace mf
