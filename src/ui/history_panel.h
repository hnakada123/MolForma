#pragma once
#include "nwchem/history.h"
#include <QWidget>
class QComboBox;
class QLineEdit;
class QCheckBox;
class QTableWidget;
class QPlainTextEdit;
class QTabWidget;
class QPushButton;
class QLabel;
namespace mf {
class MainWindow;
class HistoryPanel : public QWidget {
    Q_OBJECT
  public:
    explicit HistoryPanel(MainWindow *owner);
    void refresh();
    void setRoot(const QString &path, bool remember = true);
    void selectDirectory(const QString &path);
    void notifyJob(const QString &path);
    QString selectedDirectory() const;
    bool scanning() const {
        return scan.active();
    }
    void reuseSelected();
    void exportCsv(const QString &path) const;

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    friend class McpGui;
    MainWindow *window;
    HistoryScan scan;
    QTimer timer;
    QVector<HistoryEntry> entries;
    QString root, pendingSelection;
    QString reference; // Job directory whose final energy ΔE refers to.
    QSet<QString> changedJobs;
    bool followsExecution = true;
    QLineEdit *rootField, *search;
    QComboBox *filter, *energyUnit;
    QCheckBox *follow;
    QTableWidget *table;
    QTabWidget *details;
    QPlainTextEdit *summary, *input, *output, *errors, *record;
    QLabel *status;
    QPushButton *cancel, *refreshButton, *reuse, *viewer, *folder, *stop, *trash, *csv, *setReference;
    const HistoryEntry *selectedEntry() const;
    QString activeDirectory() const;
    void populate(bool reloadDetails = true);
    void showDetails();
    void updateActions();
    void updateLogs();
    void openResult();
    void trashSelected();
};
} // namespace mf
