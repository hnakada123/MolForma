// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/history.h"
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <optional>
class QAction;
class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
namespace mf {
class MainWindow;
class ChoiceButtons;
class ElidedLabel;
class RunProgress;
class SettingsCard;
// "履歴": the jobs of a job folder at the left, by day while sorted by their time, with the files of
// the selected job below them; at the right the selected job (its energy, or the progress of the run
// of this window, the facts of its record, a memo and the actions). Comparing adds a column of ΔE
// against a reference job, for the jobs of the same composition, method, basis and kind of energy.
class HistoryPanel : public QWidget {
    Q_OBJECT
  public:
    explicit HistoryPanel(MainWindow *owner);
    void refresh();
    void setRoot(const QString &path, bool remember = true);
    QString rootDirectory() const {
        return root;
    }
    void selectDirectory(const QString &path);
    void notifyJob(const QString &path);
    QString selectedDirectory() const;
    bool scanning() const {
        return scan.active();
    }
    void reuseSelected();
    // The listed jobs in their order, as the CSV export writes them.
    QVector<HistoryEntry> visibleEntries() const;
    void exportCsv(const QString &path) const;
    void setComparing(bool on);
    void applyChromeStyle();

  protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    friend class McpGui;
    struct Row {
        const HistoryEntry *entry;
        bool owned;                  // The run of this window.
        std::optional<double> delta; // ΔE / Hartree against the reference.
        QString note;                // Why there is no ΔE.
    };
    MainWindow *window;
    HistoryScan scan;
    QTimer timer, memoTimer;
    QVector<HistoryEntry> entries;
    QStringList listed; // The job folders in the list, in their order.
    QString root, pendingSelection;
    QString reference;     // The job folder whose energy ΔE refers to.
    QString memoDirectory; // The job whose memo is in the memo field.
    QSet<QString> changedJobs;
    QHash<QString, std::optional<HistoryMolecule>> molecules;
    bool followsExecution = true, scanned = false, comparing = false, loadingMemo = false;
    bool filesOpen = true, filesOpenBeforeComparing = true, grouped = true;
    int sortColumn = 4;
    int filesHeight = 240;
    Qt::SortOrder sortOrder = Qt::DescendingOrder;
    // Toolbar and banner.
    QToolButton *rootButton;
    QAction *browseAction, *executionRootAction;
    QLineEdit *search;
    QComboBox *filter, *energyUnit, *fileChoice;
    ChoiceButtons *filterButtons, *unitButtons;
    QPushButton *compare, *cancel, *refreshButton, *repairAll;
    QAction *csv;
    QFrame *orphanBanner;
    QLabel *orphanText;
    // The list and the files.
    QStackedWidget *pages, *fileStack;
    QSplitter *listSplit;
    QTreeWidget *table;
    QLabel *status, *note;
    SettingsCard *filesCard;
    QLabel *fileName;
    QCheckBox *follow;
    QToolButton *copyFile, *filesToggle;
    QPlainTextEdit *input, *output, *errors, *record;
    // The selected job.
    QWidget *detail;
    QLabel *detailEmpty, *statePill, *level, *facts;
    ElidedLabel *title;
    QFrame *energyBox, *progressBox, *repairBox, *trashRow;
    QLabel *energyCaption, *energyValue, *energySuffix, *energyNote, *progressTiming;
    RunProgress *progress;
    QGridLayout *factGrid, *actionGrid;
    QWidget *memoBox;
    QPlainTextEdit *memo;
    QLabel *memoStatus, *trashNote;
    QPushButton *viewer, *reuse, *folder, *stop, *repair, *trash, *setReference, *showExecution,
        *stopComparing;
    // The empty folder.
    QLabel *emptyTitle, *emptyText, *emptyExecution;
    QPushButton *emptyBack;
    const HistoryEntry *entry(const QString &directory) const;
    const HistoryEntry *selectedEntry() const;
    QString activeDirectory() const;
    QString executionRoot() const;
    void populate(bool reloadDetails = true);
    void fillRow(QTreeWidgetItem *item, const Row &row, bool grouped, double largest) const;
    void updateRow(const HistoryEntry &e);
    void updateToolbar();
    void showEmptyPage();
    void showDetails();
    void showEnergy(const HistoryEntry &e, bool owned);
    void showFacts(const HistoryEntry &e, bool owned);
    void arrangeActions(const HistoryEntry *e, bool owned);
    void updateLive();
    void updateActions();
    void updateLogs();
    void setFilesOpen(bool open, bool remember = true);
    void fitDetail();
    void saveMemo();
    void openResult();
    void trashSelected();
    void repairSelected();
    void repairOrphans();
};
} // namespace mf
