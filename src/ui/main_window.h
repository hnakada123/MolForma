#pragma once
#include "mcp/policy.h"
#include "ui/molecule_view.h"
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
namespace mf {
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

  protected:
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
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
    QTabWidget *workspaceTabs = nullptr;
    QComboBox *tool = nullptr, *unitBox = nullptr, *category = nullptr, *elementBox = nullptr,
              *bondKind = nullptr;
    QLineEdit *search = nullptr;
    QListWidget *palette = nullptr, *residueList = nullptr, *pointList = nullptr;
    QTabWidget *properties = nullptr, *editPages = nullptr;
    QLabel *summary = nullptr, *measurement = nullptr, *candidateLabel = nullptr, *sequenceLabel = nullptr;
    QWidget *candidateBar = nullptr, *chargeEditor = nullptr;
    QLabel *chargeSummary = nullptr;
    QCheckBox *includePointCharges = nullptr;
    QCheckBox *deleteTerminate = nullptr; // "削除後に水素で終端化" in the edit panel.
    QWidget *moleculePage = nullptr, *banner = nullptr;
    QLabel *bannerLabel = nullptr;
    QPushButton *undoButton = nullptr, *redoButton = nullptr;
    QAction *undoAction = nullptr, *redoAction = nullptr;
    QComboBox *labelModeBox = nullptr;
    QCheckBox *autoH = nullptr, *keepSelection = nullptr, *keepTool = nullptr, *showH = nullptr,
              *growth = nullptr;
    QPushButton *cancelWorkerButton = nullptr, *periodicTableButton = nullptr;
    QString paletteElement = "C";
    QDoubleSpinBox *dimension = nullptr;
    QJsonArray customParts;
    QDoubleSpinBox *positionFields[3]{}, *translateFields[3]{}, *duplicateFields[3]{}, *chargeFields[3]{};
    QDoubleSpinBox *twist = nullptr, *distance = nullptr, *chargeValue = nullptr;
    QSpinBox *formal = nullptr, *radical = nullptr, *oxidation = nullptr, *totalCharge = nullptr,
             *multiplicity = nullptr;
    QLineEdit *mass = nullptr, *siteInput = nullptr, *sequenceInput = nullptr;
    QComboBox *geometryBox = nullptr, *aminoState = nullptr, *chiralityBox = nullptr, *nTerm = nullptr,
              *cTerm = nullptr;
    QString growthResidue;
    bool growthN = false;
    void buildUi();
    void buildMenus();
    void buildEditPanel();
    void buildCoordinationPanel();
    void buildResiduePanel();
    void buildChargePanel();
    void refresh();
    void refreshPalette();
    void refreshSelection();
    void refreshChargePanel();
    QFormLayout *panel(const QString &title);
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
    void place(const QString &atom, const Vec &position);
    void atomClicked(const QString &id, Qt::KeyboardModifiers modifiers);
    Ids expandedSelection(const Ids &ids) const;
    void setState(const Document &d, bool trusted = true);
    // Paths reached by an MCP-driven GUI action obey --mcp-allow-dir like the semantic tools.
    void checkMcpPath(const QString &path) const;
    void trackExecution(const Document &before, const Document &after);
    void commit(const QString &name, const Document &next, bool remote = false);
    void newDocument();
    void open();
    bool save(bool as = false);
    bool resolveCandidate();
    bool confirmDiscard();
    void exportFile();
    // Asks which structure of a multi-frame XYZ file to read (0-based; 0 for single frames).
    int chooseXyzFrame(const QString &path);
    void importSmiles();
    void addFragmentFromSmiles();
    void addFragmentFromFile();
    // mode: smiles, fragment, refine, regenerate or conformers (count: conformers to keep).
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {}, int count = 10);
    void cancelWorker();
    void contextMenu(const QPoint &p, const QString &id);
    void coordinateDialog();
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
    void positionBanner();
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
