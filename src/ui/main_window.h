// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/chemistry_job.h"
#include "editing/editor_session.h"
#include "editing/xtb.h"
#include "io/document_storage.h"
#include "mcp/policy.h"
#include "ui/editor_context.h"
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
class ConformerSetDialog;
class MainWindow : public QMainWindow,
                   public EditorContext,
                   public UiServices,
                   public WorkspaceActions,
                   public ExecutionAccess {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    const Document &document() const override {
        return session.document();
    }
    const Document &displayDocument() const override {
        return session.displayDocument();
    }
    // Grows with every change of the document or its candidate (EditorSession::revision).
    quint64 revision() const override {
        return session.revision();
    }
    bool hasCandidate() const override {
        return session.candidate().has_value();
    }
    void edit(const QString &name, const std::function<void(Document &)> &operation,
              bool preview = false) override;
    void confirmCandidate() override;
    void cancelCandidate() override;
    void remoteHistory(const QString &action);
    // Without ask, the choices of what to read are those the dialogs propose (a screenshot from the command
    // line).
    void loadPath(const QString &path, bool ask = true);
    bool chemistryRunning() const override {
        return chemistry.active();
    }
    QString projectPath() const override {
        return storage.path();
    }
    void clearProjectPath() {
        storage.setPath({});
    }
    void setSelection(const Ids &ids) override;
    void loadDocument(const Document &d);
    // Ask about unsaved edits, then replace the project with an unsaved copy and reset its history.
    bool replaceUnsavedDocument(const Document &d) override;
    QUndoStack *history() {
        return &undo;
    }
    MoleculeView *canvas() {
        return view;
    }
    void safe(const std::function<void()> &operation) override;
    void showMessage(const QString &message, int timeout = 0) override;
    void addStatusWidget(QWidget *widget) override;
    void fitMoleculeView() override;
    // Interactive sessions: periodic autosave of unsaved work, and recovery after a crash.
    // The copies go to <AppLocalDataLocation>/autosave, on Windows and macOS too under XDG_DATA_HOME when it
    // is set.
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
    void showCoordinateTable() override;
    // A practice file of the recipes, embedded with the manual: a part (.molforma-part.json) joins
    // "マイ部品"; a molecule is saved as a copy (asked where when target is empty) and opened. Returns
    // whether it was used.
    bool useSample(const QString &resource, const QString &target = {});

    // The editor context used by panels and the MCP adapter. Widgets and mutable session state
    // remain private; consumers request operations and observe snapshots.
    const std::optional<Document> &candidate() const {
        return session.candidate();
    }
    const Ids &selection() const override;
    bool modified() const {
        return !undo.isClean();
    }
    QVariant preference(const QString &key, const QVariant &fallback = {}) const override {
        return settings.value(key, fallback);
    }
    void setPreference(const QString &key, const QVariant &value) override {
        settings.setValue(key, value);
    }
    ExecutionPanel *executionPanel() const {
        return execution;
    }
    ResultsPanel *resultsPanel() const {
        return results;
    }
    HistoryPanel *historyPanel() const {
        return calculationHistory;
    }
    void showPanel(QWidget *panel) override;
    void showPanel(int index) override;
    // Routes a setting to its owning panel. Tab indices are zero-based, as in showPanel().
    // Memory belongs to execution; detailed groups belong to calculation settings.
    void showSetting(int tab, const QString &setting = {}, const QString &group = {}) override;
    QString settingSectionTitle(const QString &setting) const override;
    void showInputLine(int line = -1) override;
    void commitPendingSettings() override;
    QWidget *currentPanel() const;
    int currentPanelIndex() const;
    QString moleculeLayoutMode() const override;
    void setPanelDiagnostics(QWidget *panel, bool error, const QString &tip) override;
    void setRunPanelShown(bool shown) override;
    const McpPolicy *accessPolicy() const override {
        return mcp.policy;
    }
    void setAccessPolicy(const McpPolicy *policy) {
        mcp.policy = policy;
    }
    bool remoteRequest() const override {
        return mcp.depth > 0;
    }
    QScopedValueRollback<int> remoteScope() {
        return QScopedValueRollback<int>(mcp.depth, mcp.depth + 1);
    }
    // Keys of protectedExecution() whose current values the user has neither entered in MolForma,
    // confirmed before a run, nor saved in Preferences: runs with them need a confirmation.
    QStringList untrustedExecution() const override;
    void trustCurrentExecution() override;

    // Paths reached by an MCP-driven GUI action obey --mcp-allow-dir like the semantic tools.
    void checkMcpPath(const QString &path) const override;
    // Sorts the atoms: "element", "residue" or "selected" (the selection first).
    void sortAtoms(const QString &mode) override;
    bool confirmDiscard();
    bool resolveCandidate() override;
    void refreshPanel(QWidget *panel, bool always = false) override;
    // The header while a run is active: the elapsed time, and stop instead of run.
    void showRun(bool active, const QString &text, bool stoppable) override;
    void cancelWorker() override;
    // mode: smiles, fragment, refine, regenerate or conformers (count: conformers to keep).
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {},
                   int count = 10) override;
    // Optimizes the structure with xtb (the program named in Preferences, "xtb/executable"); the
    // result becomes a candidate. MCP cannot choose the program.
    void runXtb(const XtbOptions &options);
    void xtbDialog();
    // The structure the result viewer shows; fails while it shows none.
    Document resultStructure() const override;
  signals:
    void panelChanged();

  protected:
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    EditorSession session;
    // MCP access, set by McpGui.
    struct McpAccess {
        const McpPolicy *policy = nullptr;
        int depth = 0; // > 0 while an MCP request or GUI command is being applied
    } mcp;
    // Values of the executable, launcher, environment and additional NWChem input (per key of
    // protectedExecution) that the user entered in MolForma or confirmed before a run. Values read
    // from a project, an autosave copy or the history, and those set over MCP, count only once
    // confirmed; those saved in Preferences always count.
    QHash<QString, QList<QJsonValue>> trustedExecution;
    QMenu *recentMenu = nullptr;
    QUndoStack &undo = session.history();
    QSettings settings;
    DocumentStorage storage;
    ChemistryJob chemistry;
    ConformerSetDialog *conformerSet = nullptr;
    ConformerSetDialog *conformerSetDialog();
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
    void trustExecution(const QJsonObject &values);
    void newDocument();
    void open();
    // Downloads a PDB entry by its ID from RCSB PDB into the cache folder and opens it.
    void downloadPdb();
    // "PubChemから取得…": a compound by its name or CID, as a new molecule.
    void downloadPubchem();
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
