#pragma once
#include "mcp/policy.h"
#include "ui/molecule_view.h"
#include <QHash>
#include <QLockFile>
#include <QMainWindow>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
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
class QStackedWidget;
class QToolButton;
class QActionGroup;
class QFrame;
class QVBoxLayout;
namespace mf {
class HeaderBar;
class SettingsCard;
class ViewControls;
class CoordinatesPanel;
class CalculationPanel;
class AdvancedPanel;
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
        return doc;
    }
    const Document &displayDocument() const {
        return candidate ? *candidate : doc;
    }
    bool hasCandidate() const {
        return candidate.has_value();
    }
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview = false);
    void confirmCandidate();
    void cancelCandidate();
    void remoteHistory(const QString &action);
    void loadPath(const QString &path);
    void setSelection(const Ids &ids);
    void loadDocument(const Document &d);
    QUndoStack *history() {
        return &undo;
    }
    MoleculeView *canvas() {
        return view;
    }
    void safe(const std::function<void()> &operation);
    // Interactive sessions: periodic autosave of unsaved work, and recovery after a crash.
    void startAutosave();
    void offerRecovery();
    // Writes the recovery copy now if there is unsaved work (the timer calls it every minute).
    void autosave();
    // Recently opened or saved files (QSettings "recentFiles"), newest first.
    QStringList recentFiles() const;
    // Brings tabs that were hidden during edits up to date (McpGui calls it before using the GUI).
    void refreshStalePanels();

  protected:
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    friend class DocumentCommand;
    friend class CoordinatesPanel;
    friend class CalculationPanel;
    friend class AdvancedPanel;
    friend class ExecutionPanel;
    friend class InputPreviewPanel;
    friend class ResultsPanel;
    friend class HistoryPanel;
    friend class McpGui;
    Document doc;
    std::optional<Document> candidate;
    // MCP access, set by McpGui. A calculation started over MCP may only use the executable,
    // launcher and environment that were last changed outside MCP (trustedExecution).
    struct McpAccess {
        const McpPolicy *policy = nullptr;
        int depth = 0; // > 0 while an MCP request or GUI command is being applied
        QJsonObject trustedExecution;
    } mcp;
    QString candidateName, path;
    bool candidateRemote = false, workerRemote = false; // Candidate or worker started over MCP.
    Ids selected;
    QVector<Ids> monitors; // Measurements kept on screen (view state, not saved).
    QMenu *recentMenu = nullptr;
    QTimer *autosaveTimer = nullptr;
    QString autosavePath;
    std::unique_ptr<QLockFile> autosaveLock;
    QByteArray autosavedIdentity; // Content of the last recovery copy.
    QUndoStack undo;
    QSettings settings;
    QProcess *worker = nullptr;
    std::unique_ptr<QTemporaryDir> workdir;
    QByteArray workerBase;
    bool refreshing = false;
    MoleculeView *view = nullptr;
    CoordinatesPanel *coordinates = nullptr;
    CalculationPanel *calculation = nullptr;
    AdvancedPanel *advanced = nullptr;
    ExecutionPanel *execution = nullptr;
    InputPreviewPanel *inputPreview = nullptr;
    ResultsPanel *results = nullptr;
    HistoryPanel *calculationHistory = nullptr;
    // The tabs; their bar is hidden and the workflow steps of the header switch them.
    QTabWidget *workspaceTabs = nullptr;
    HeaderBar *header = nullptr;
    QWidget *workflowSteps = nullptr;
    QButtonGroup *stepGroup = nullptr; // One button per tab, its index as id.
    QToolButton *documentButton = nullptr, *runButton = nullptr, *headerStop = nullptr;
    // "実行中 18:31" in the header while NWChem runs; it opens the run panel.
    QToolButton *runPill = nullptr;
    // The header while a run is active: the elapsed time, and stop instead of run.
    void showRun(bool active, const QString &text, bool stoppable);
    QButtonGroup *toolGroup = nullptr; // The editing tools in the rail at the left of "1 分子".
    QComboBox *toolBondKind = nullptr; // In the option bar of the bond and place tools; mirrors bondKind.
    QButtonGroup *unitGroup = nullptr, *categoryGroup = nullptr; // Selection unit, palette category.
    QComboBox *elementBox = nullptr, *bondKind = nullptr;
    QLineEdit *search = nullptr;
    QListWidget *palette = nullptr, *residueList = nullptr, *pointList = nullptr;
    // The pages at the right of "1 分子"; segments above them switch the hidden tab bar.
    QTabWidget *properties = nullptr;
    QButtonGroup *propertyGroup = nullptr;
    ViewControls *viewControls = nullptr;
    // The cards of a page of properties by the form that adds to it, and the row of buttons that
    // button() adds to (its row in the form).
    QHash<QFormLayout *, QVBoxLayout *> pageCards;
    QHash<QFormLayout *, std::pair<int, QWidget *>> buttonRows;
    QLabel *summary = nullptr, *measurement = nullptr, *candidateLabel = nullptr, *sequenceLabel = nullptr,
           *mouseHint = nullptr, *measureTitle = nullptr, *dimensionLabel = nullptr,
           *selectionTitle = nullptr, *selectionList = nullptr, *candidateTitle = nullptr,
           *candidateChanges = nullptr, *toolPart = nullptr;
    QWidget *candidateBar = nullptr, *chargeEditor = nullptr;
    // The charge page: the calculation's charge and point-charge settings ("3 計算条件"), shown.
    QLabel *chargeSummary = nullptr, *chargeState = nullptr, *pointChargeState = nullptr,
           *inputState = nullptr;
    QCheckBox *deleteTerminate = nullptr;      // "削除後に水素で終端化" in the edit panel,
    QCheckBox *toolDeleteTerminate = nullptr;  // and the same in the option bar of the delete tool.
    QDoubleSpinBox *toolChargeValue = nullptr; // The point-charge tool's charge; mirrors chargeValue.
    QWidget *moleculePage = nullptr, *banner = nullptr;
    // Above the 3D view: the options of the current tool, or the candidate in their place.
    QStackedWidget *topRow = nullptr;
    QHash<QString, QWidget *> toolOptionGroups; // Option bar groups by object name.
    QWidget *sequenceChip = nullptr, *measureCard = nullptr, *candidateCard = nullptr;
    QLabel *bannerLabel = nullptr;
    QPushButton *undoButton = nullptr, *redoButton = nullptr;
    QAction *undoAction = nullptr, *redoAction = nullptr, *confirmAction = nullptr, *stopAction = nullptr,
            *runPanelAction = nullptr;
    QAction *showHydrogens = nullptr, *showIndices = nullptr;
    QActionGroup *labelModes = nullptr;
    QCheckBox *autoH = nullptr, *keepSelection = nullptr, *keepTool = nullptr, *growth = nullptr;
    QPushButton *cancelWorkerButton = nullptr, *periodicTableButton = nullptr;
    bool stylingChrome = false;
    QString paletteElement = "C";
    QDoubleSpinBox *dimension = nullptr;
    QJsonArray customParts;
    QDoubleSpinBox *positionFields[3]{}, *translateFields[3]{}, *duplicateFields[3]{}, *chargeFields[3]{};
    QDoubleSpinBox *twist = nullptr, *distance = nullptr, *chargeValue = nullptr;
    QSpinBox *formal = nullptr, *radical = nullptr, *oxidation = nullptr;
    QLineEdit *mass = nullptr, *siteInput = nullptr, *sequenceInput = nullptr;
    QComboBox *geometryBox = nullptr, *aminoState = nullptr, *chiralityBox = nullptr, *nTerm = nullptr,
              *cTerm = nullptr;
    QString growthResidue;
    bool growthN = false;
    void buildUi();
    void buildHeader();
    void buildMenus();
    // Style sheets of the header, rail, option bar and overlays from the current palette.
    void applyChromeStyle();
    // The header's steps follow the tabs: the open one, and the error mark of "6 入力プレビュー".
    void syncWorkflowSteps();
    QString selectionUnit() const;
    QString paletteCategory() const;
    void setPaletteCategory(const QString &code);
    void buildEditPanel();
    void buildCoordinationPanel();
    void buildResiduePanel();
    void buildChargePanel();
    void refresh();
    void refreshTitle();
    void refreshSummary();
    // The tabs "2 座標" to "6 入力プレビュー"; while the window is shown, only the open one refreshes
    // on each change and the others are marked stale until opened.
    QList<QWidget *> workspacePanels() const;
    QSet<QWidget *> stalePanels;
    void refreshPanels();
    void refreshPanel(QWidget *panel, bool always = false);
    void refreshPalette();
    void refreshSelection();
    void refreshChargePanel();
    // Controls that act on the selection: enabled for minimum..maximum selected atoms (0: no
    // upper limit), with a tooltip that says what to select while disabled.
    struct SelectionControl {
        QWidget *widget;
        int minimum, maximum;
        QString tooltip;
    };
    QVector<SelectionControl> selectionControls;
    void enableWithSelection(QWidget *widget, int minimum, int maximum = 0);
    void refreshSelectionControls();
    // Delete, the bond-order keys and similar: a missing selection is a hint, not an error dialog.
    bool hasSelection(int count = 0);
    // A page of the properties at the right of "1 分子", and the form for what comes before its cards.
    QFormLayout *panel(const QString &title);
    // A card on the page of form, which then adds to the card. open: 1 or 0 for a card that opens and
    // closes (open or closed at first), -1 for one always open.
    SettingsCard *section(QFormLayout *&form, const QString &title, int open = -1);
    void showProperties(const QString &title);
    QPushButton *button(QFormLayout *form, const QString &text, const std::function<void()> &slot,
                        const QString &objectName = {});
    QDoubleSpinBox *number(QFormLayout *form, const QString &label, double value = 0, double minimum = -10000,
                           double maximum = 10000);
    QComboBox *choices(QFormLayout *form, const QString &label, const QStringList &labels,
                       const QStringList &values = {});
    Ids requireSelection(int count = 0) const;
    QString selectedResidue() const;
    QString selectedCenter() const;
    QString selectedPart() const;
    QList<int> sites() const;
    QJsonObject currentTemplate() const;
    void paletteChosen();
    void selectFirstPart(); // The first palette row that is a part, not a group heading.
    // Chooses an element for the atom tool, as from the periodic table or the common-element buttons.
    void chooseElement(const QString &symbol);
    void place(const QString &atom, const Vec &position);
    void atomClicked(const QString &id, Qt::KeyboardModifiers modifiers);
    Ids expandedSelection(const Ids &ids) const;
    void setState(const Document &d, bool trusted = true);
    // Status bar note after Undo/Redo, naming the tab whose content changed.
    void announceHistory(const QString &verb, const QString &name, const Document &from, const Document &to);
    // Paths reached by an MCP-driven GUI action obey --mcp-allow-dir like the semantic tools.
    void checkMcpPath(const QString &path) const;
    void trackExecution(const Document &before, const Document &after);
    bool commit(const QString &name, const Document &next, bool remote = false);
    void newDocument();
    void open();
    bool save(bool as = false);
    bool resolveCandidate();
    bool confirmDiscard();
    void exportFile();
    // Asks which structure of a multi-frame XYZ file to read (0-based; 0 for single frames).
    int chooseXyzFrame(const QString &path);
    QString askSmiles(const QString &title, const QString &label, const QString &initial);
    void importSmiles(const QString &initial = {});
    void addFragmentFromSmiles(const QString &initial = {});
    void addFragmentFromFile();
    // mode: smiles, fragment, refine, regenerate or conformers (count: conformers to keep).
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {}, int count = 10);
    void cancelWorker();
    void contextMenu(const QPoint &p, const QString &id);
    void savePart();
    void loadPart();
    void selectNumbers();
    void showReport();
    void showManual();
    void showShortcuts();
    // The one deletion of the editor: the atoms, hydrogens bonded only to them, and (when
    // "削除後に水素で終端化" is on) new hydrogens on the cut atoms, shown as a candidate.
    void deleteAtoms(const Ids &ids);
    void selectAll();
    void copySelection();
    void pasteFragment();
    // The editing tool ("select", "place", "replace", …) and the hint under the 3D view for it.
    QString currentTool() const;
    void setTool(const QString &code);
    // What "配置" puts down, from the palette's chosen item: "atom", "center", "part" (skeletons,
    // amino acids, caps), "substituent", "ligand" or "smiles".
    QString placementKind() const;
    // The hint under the 3D view and the option bar groups for the current tool.
    void refreshToolHint();
    void addRecent(const QString &path);
    void refreshRecent();
    void removeAutosave();
    void openDropped(const QString &path);
    void symmetryDialog();
    void zMatrixDialog();
    void compareDialog();
    void preferences();
    void pdbPreparation();
    void addAmino(const QString &rid, bool n);
};
} // namespace mf
