// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/editor_context.h"
#include "ui/editor_tool.h"
#include "ui/molecule_view.h"
#include "ui/selection_controls.h"
#include <QHash>
#include <QWidget>
class QStatusBar;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QLabel;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
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
namespace mf {
class CandidateControls;
class SettingsCard;
class ViewControls;
class CoordinatesPanel;
class SequenceTrack;
class PartPalette;
class PropertyPage;
class TransformPage;
class AttributesPage;
class ChargePage;

// Owns the molecular workspace as the composer of its parts: the 3D view with the tools and overlays,
// the selection, the layouts with the coordinate table (CoordinatesPanel), and the pages of properties
// that hold the options of its tools ("編集", "配位", "残基": molecule_properties.cpp). It connects the
// parts that are widgets of their own: the palette with the user's parts (PartPalette), the pages
// that need only the editor context and the selection (TransformPage, AttributesPage, ChargePage on
// the PropertyPage they share) and the dialogs of the tools (molecule_dialogs.h).
// Document transactions stay in the application session, which this panel does not see: it reads the
// document and its candidate through the editor context and asks that for edits.
class MoleculePanel : public QWidget {
    Q_OBJECT
  public:
    MoleculePanel(EditorContext &editorContext, UiServices &uiContext, WorkspaceActions &workspace,
                  QWidget *parent = nullptr);
    MoleculeView *canvas() const {
        return view;
    }
    CoordinatesPanel *coordinatePanel() const {
        return coordinates;
    }
    const Ids &selection() const {
        return selected;
    }
    void setSelection(const Ids &ids);
    void pruneSelection(const Document &document);
    bool keepsSelection() const;
    void cancelGrowth();
    void savePreferences();
    void stopEditing();
    void refresh();
    void applyChromeStyle();
    void setWorkerRunning(bool running);
    void addEditActions(QMenu *menu);
    void addViewActions(QMenu *menu);
    // Shows "1 分子" with the table of the coordinates (under the 3D view, unless it is shown already).
    void showCoordinateTable();
    // Sorts the atoms: "element", "residue" or "selected" (the selection first).
    void sortAtoms(const QString &mode);
    void importSmiles(const QString &initial = {});
    void addFragmentFromSmiles(const QString &initial = {});
    void addFragmentFromFile();
    void loadPartFile(const QString &path);
    void showReport();
    void compareDialog();
    void zMatrixDialog();
    void symmetryDialog();
    void solvationDialog();
  signals:
    void openRequested();
    void downloadRequested();

  private:
    EditorContext &editorContext;
    UiServices &uiContext;
    WorkspaceActions &workspace;
    Ids selected;
    QVector<Ids> monitors;
    bool refreshing = false;
    MoleculeView *view = nullptr;
    CoordinatesPanel *coordinates = nullptr;
    QWidget *propertyColumn = nullptr;
    QButtonGroup *toolGroup = nullptr; // The editing tools in the rail at the left of "1 分子".
    QComboBox *toolBondKind = nullptr; // In the option bar of the bond and place tools; mirrors bondKind.
    QButtonGroup *unitGroup = nullptr; // Selection unit.
    QComboBox *elementBox = nullptr, *bondKind = nullptr;
    QListWidget *residueList = nullptr;
    // The pages at the right of "1 分子"; segments above them switch the hidden tab bar.
    QTabWidget *properties = nullptr;
    QButtonGroup *propertyGroup = nullptr;
    QList<PropertyPage *> propertyPages; // In the order of their tabs.
    TransformPage *transformPage = nullptr;
    AttributesPage *attributesPage = nullptr;
    ChargePage *chargePage = nullptr;
    ViewControls *viewControls = nullptr;
    QLabel *measurement = nullptr, *mouseHint = nullptr, *measureTitle = nullptr, *dimensionLabel = nullptr,
           *selectionTitle = nullptr, *selectionList = nullptr, *candidateChanges = nullptr,
           *toolPart = nullptr;
    QCheckBox *deleteTerminate = nullptr;      // "削除後に水素で終端化" in the edit panel,
    QCheckBox *toolDeleteTerminate = nullptr;  // and the same in the option bar of the delete tool.
    QDoubleSpinBox *toolChargeValue = nullptr; // The point-charge tool's charge; mirrors the charge page's.
    CandidateControls *banner = nullptr;
    // Above the 3D view: the options of the current tool, or the candidate in their place.
    QStackedWidget *topRow = nullptr;
    QSplitter *moleculeSplit = nullptr;         // The 3D view over the coordinate table.
    QComboBox *moleculeLayout = nullptr;        // "view", "both" or "table", behind the segments under them.
    SettingsCard *positionCard = nullptr;       // "座標（1原子）", hidden while the table is shown.
    PartPalette *partPalette = nullptr;         // The palette of parts, folded with
    QToolButton *paletteToggle = nullptr;       // "パレット" at the bottom of the tool rail.
    QHash<QString, QWidget *> toolOptionGroups; // Option bar groups by object name.
    SequenceTrack *sequence = nullptr;          // The residues between the options and the 3D view.
    QWidget *measureCard = nullptr, *candidateCard = nullptr;
    QAction *showHydrogens = nullptr, *showIndices = nullptr;
    QActionGroup *labelModes = nullptr;
    QCheckBox *autoH = nullptr, *keepSelection = nullptr, *keepTool = nullptr;
    QPushButton *cancelWorkerButton = nullptr;
    QDoubleSpinBox *dimension = nullptr;
    QDoubleSpinBox *positionFields[3]{};
    QDoubleSpinBox *distance = nullptr;
    QSpinBox *oxidation = nullptr;
    QLineEdit *siteInput = nullptr, *sequenceInput = nullptr;
    QComboBox *geometryBox = nullptr, *aminoState = nullptr, *chiralityBox = nullptr, *nTerm = nullptr,
              *cTerm = nullptr;
    QString growthResidue;
    bool growthN = false;
    int ligandCandidate = 0; // The candidate serial of the ligand proposed last (proposeLigand).
    // The parts of the workspace, built once by the constructor in this order.
    QWidget *buildToolRail();
    QWidget *buildCenter(); // The options or the candidate, the sequence, the 3D view over the table.
    QWidget *buildOptionBar();
    QWidget *buildViewArea();
    QWidget *buildHintBar();
    QWidget *buildPropertyColumn();
    void buildPropertyPages();
    void buildViewActions();
    void connectTools();
    void connectView();
    // The pages of properties the workspace fills itself (molecule_properties.cpp), each a page added
    // with addPage().
    PropertyPage *addPage(const QString &title);
    void buildEditPanel();
    void buildCoordinationPanel();
    void buildResiduePanel();
    // The controls outside the pages that need atoms selected (the measurement over the view).
    SelectionControls selectionControls;
    void refreshSelectionControls();
    // Delete, the bond-order keys and similar: a missing selection is a hint, not an error dialog.
    bool hasSelection(int count = 0);
    void showProperties(const QString &title);
    Ids requireSelection(int count = 0) const;
    QString selectedResidue() const;
    QString selectedCenter() const;
    QString selectedPart() const;
    QList<int> sites() const;
    QJsonObject currentTemplate() const; // The part chosen in the palette.
    // The palette's part was chosen: takes the tool that places it and shows the page of its options.
    void paletteChosen(const PaletteEntry &entry);
    void place(const QString &atom, const Vec &position);
    void atomClicked(const QString &id, Qt::KeyboardModifiers modifiers);
    Ids expandedSelection(const Ids &ids) const;
    void applyMoleculeLayout();
    QString selectionUnit() const;
    void showPalette(bool show);
    void refreshSelection();
    void contextMenu(const QPoint &p, const QString &id);
    void selectNumbers();
    void deleteAtoms(const Ids &ids);
    // The edits that the tools, buttons, keys and menus share, with the options of the panel.
    void bondAtoms(const QString &a, const QString &b, const QString &kind);
    void replaceElements(const Ids &ids, const QString &symbol);
    // A ligand placed as a candidate; clicking the sites for the next one confirms it.
    void proposeLigand(const QString &center, const QList<int> &sites, const QJsonObject &t,
                       double twist = 0);
    void selectAll();
    void copySelection();
    void pasteFragment();
    EditorTool tool() const;
    void setTool(EditorTool tool);
    PaletteEntry paletteChoice() const; // What the palette row chosen places.
    void refreshToolHint();
    void addAmino(const QString &rid, bool n);

    const Document &document() const {
        return editorContext.document();
    }
    const Document &displayDocument() const {
        return editorContext.displayDocument();
    }
    bool hasCandidate() const {
        return editorContext.hasCandidate();
    }
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview = false);
    void safe(const std::function<void()> &operation);
    void showMessage(const QString &message, int timeout = 0) const;
    bool chemistryRunning() const;
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {}, int count = 10);
    void cancelWorker();
    void confirmCandidate();
    void cancelCandidate();
    bool resolveCandidate();
    void checkMcpPath(const QString &path) const;
};
} // namespace mf
