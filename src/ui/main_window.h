#pragma once
#include "editing/chemistry_job.h"
#include "editing/editor_session.h"
#include "io/document_storage.h"
#include "mcp/policy.h"
#include "ui/molecule_view.h"
#include <QHash>
#include <QMainWindow>
#include <QScopedValueRollback>
#include <QSettings>
#include <QUndoStack>
#include <functional>
#include <optional>
class QComboBox;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QFormLayout;
class QPushButton;
class QTableWidget;
class QMenu;
class QTimer;
class QButtonGroup;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QActionGroup;
class QFrame;
class QVBoxLayout;
namespace mf {
class MoleculePanel;
class CandidateControls;
class HeaderBar;
class SettingsCard;
class ViewControls;
class CoordinatesPanel;
class CalculationPanel;
class ExecutionPanel;
class InputPreviewPanel;
class ResultsPanel;
class HistoryPanel;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    const Document &document() const {
        return session.document();
    }
    const Document &displayDocument() const {
        return session.displayDocument();
    }
    bool hasCandidate() const {
        return session.candidate().has_value();
    }
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview = false);
    void confirmCandidate();
    void cancelCandidate();
    void remoteHistory(const QString &action);
    void loadPath(const QString &path);
    bool chemistryRunning() const {
        return chemistry.active();
    }
    QString projectPath() const {
        return storage.path();
    }
    void clearProjectPath() {
        storage.setPath({});
    }
    void setSelection(const Ids &ids);
    void loadDocument(const Document &d);
    // Ask about unsaved edits, then replace the project with an unsaved copy and reset its history.
    bool replaceUnsavedDocument(const Document &d);
    QUndoStack *history() {
        return &undo;
    }
    MoleculeView *canvas() {
        return view;
    }
    void safe(const std::function<void()> &operation);
    // Interactive sessions: periodic autosave of unsaved work, and recovery after a crash.
    // The copies go to <AppLocalDataLocation>/autosave, on Windows too under XDG_DATA_HOME when it is set.
    static QString autosaveDirectory();
    void startAutosave();
    void offerRecovery();
    // Writes the recovery copy now if there is unsaved work (the timer calls it every minute).
    void autosave();
    // Recently opened or saved files (QSettings "recentFiles"), newest first.
    QStringList recentFiles() const;
    // Brings tabs that were hidden during edits up to date (McpGui calls it before using the GUI).
    void refreshStalePanels();
    // Shows the table of the coordinates under the 3D view of "1 分子" (or in its place).
    void showCoordinateTable();
    // A practice file of the recipes, embedded with the manual: a part (.molforma-part.json) joins
    // "マイ部品"; a molecule is saved as a copy (asked where when target is empty) and opened. Returns
    // whether it was used.
    bool useSample(const QString &resource, const QString &target = {});

    // The editor context used by panels and the MCP adapter. Widgets and mutable session state
    // remain private; consumers request operations and observe snapshots.
    const std::optional<Document> &candidate() const {
        return session.candidate();
    }
    const Ids &selection() const;
    bool modified() const {
        return !undo.isClean();
    }
    QVariant preference(const QString &key, const QVariant &fallback = {}) const {
        return settings.value(key, fallback);
    }
    void setPreference(const QString &key, const QVariant &value) {
        settings.setValue(key, value);
    }
    CalculationPanel *calculationPanel() const {
        return calculation;
    }
    ExecutionPanel *executionPanel() const {
        return execution;
    }
    InputPreviewPanel *previewPanel() const {
        return inputPreview;
    }
    ResultsPanel *resultsPanel() const {
        return results;
    }
    HistoryPanel *historyPanel() const {
        return calculationHistory;
    }
    void showPanel(QWidget *panel);
    void showPanel(int index);
    // Routes a setting to its owning panel. Tab indices are zero-based, as in showPanel().
    // Memory belongs to execution; detailed groups belong to calculation settings.
    void showSetting(int tab, const QString &setting = {}, const QString &group = {});
    QString settingSectionTitle(const QString &setting) const;
    void showInputLine(int line = -1);
    void commitPendingSettings();
    QWidget *currentPanel() const;
    int currentPanelIndex() const;
    QWidget *workspaceWidget() const;
    QRect runPanelArea() const; // Available overlay area in main-window coordinates.
    bool moleculePanelVisible() const;
    QString moleculeLayoutMode() const;
    void setPanelDiagnostics(QWidget *panel, bool error, const QString &tip);
    void setRunPanelShown(bool shown);
    const McpPolicy *accessPolicy() const {
        return mcp.policy;
    }
    void setAccessPolicy(const McpPolicy *policy) {
        mcp.policy = policy;
    }
    bool remoteRequest() const {
        return mcp.depth > 0;
    }
    QScopedValueRollback<int> remoteScope() {
        return QScopedValueRollback<int>(mcp.depth, mcp.depth + 1);
    }
    const QJsonObject &trustedExecution() const {
        return mcp.trustedExecution;
    }
    void trustCurrentExecution();

    // Paths reached by an MCP-driven GUI action obey --mcp-allow-dir like the semantic tools.
    void checkMcpPath(const QString &path) const;
    // Sorts the atoms: "element", "residue" or "selected" (the selection first).
    void sortAtoms(const QString &mode);
    bool confirmDiscard();
    bool resolveCandidate();
    void refreshPanel(QWidget *panel, bool always = false);
    // The header while a run is active: the elapsed time, and stop instead of run.
    void showRun(bool active, const QString &text, bool stoppable);
    void cancelWorker();
    // mode: smiles, fragment, refine, regenerate or conformers (count: conformers to keep).
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {}, int count = 10);
  signals:
    void panelChanged();
    void workspaceGeometryChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    EditorSession session;
    // MCP access, set by McpGui. A calculation started over MCP may only use the executable,
    // launcher and environment that were last changed outside MCP (trustedExecution).
    struct McpAccess {
        const McpPolicy *policy = nullptr;
        int depth = 0; // > 0 while an MCP request or GUI command is being applied
        QJsonObject trustedExecution;
    } mcp;
    QMenu *recentMenu = nullptr;
    QUndoStack &undo = session.history();
    QSettings settings;
    DocumentStorage storage;
    ChemistryJob chemistry;
    bool refreshing = false;
    MoleculePanel *moleculePage = nullptr;
    MoleculeView *view = nullptr;
    CoordinatesPanel *coordinates = nullptr;
    CalculationPanel *calculation = nullptr;
    ExecutionPanel *execution = nullptr;
    InputPreviewPanel *inputPreview = nullptr;
    ResultsPanel *results = nullptr;
    HistoryPanel *calculationHistory = nullptr;
    QTabWidget *workspaceTabs = nullptr;
    HeaderBar *header = nullptr;
    QWidget *workflowSteps = nullptr;
    QButtonGroup *stepGroup = nullptr;
    QToolButton *documentButton = nullptr, *runButton = nullptr, *headerStop = nullptr, *runPill = nullptr;
    QLabel *summary = nullptr;
    CandidateControls *candidateBar = nullptr;
    QPushButton *undoButton = nullptr, *redoButton = nullptr;
    QAction *undoAction = nullptr, *redoAction = nullptr, *confirmAction = nullptr, *stopAction = nullptr,
            *runPanelAction = nullptr;
    bool stylingChrome = false;
    QSet<QWidget *> stalePanels;
    void buildUi();
    void buildHeader();
    void buildMenus();
    void applyChromeStyle();
    void syncWorkflowSteps();
    void refresh();
    void refreshTitle();
    void refreshSummary();
    QList<QWidget *> workspacePanels() const;
    void refreshPanels();
    void announceHistory(const QString &verb, const QString &name, const Document &from, const Document &to);
    void trackExecution(const Document &before, const Document &after);
    void newDocument();
    void open();
    bool save(bool as = false);
    void exportFile();
    void chemistryReady(const ChemistryResult &result);
    void chemistryFailed(const ChemistryRequest &request, const QString &message, bool starting);
    void showManual();
    void showShortcuts();
    void addRecent(const QString &path);
    void refreshRecent();
    void removeAutosave();
    void openDropped(const QString &path);
    void preferences();
};
} // namespace mf
