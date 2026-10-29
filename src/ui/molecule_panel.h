#pragma once
#include "editing/editor_session.h"
#include "ui/editor_tool.h"
#include "ui/molecule_view.h"
#include <QHash>
#include <QSettings>
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
class CandidateControls;
class MainWindow;
class SettingsCard;
class ViewControls;
class CoordinatesPanel;

// Owns the molecular workspace: palette, tools, selection, view, coordinate layout and properties.
// Document transactions stay in the application session; this panel only observes it.
class MoleculePanel : public QWidget {
    Q_OBJECT
  public:
    MoleculePanel(MainWindow &window, const EditorSession &session, QSettings &settings);
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
    QString layoutMode() const;
    QRect propertiesArea(const QWidget *target) const;
    void addEditActions(QMenu *menu);
    void addViewActions(QMenu *menu);
    void showCoordinateTable();
    void sortAtoms(const QString &mode);
    void importSmiles(const QString &initial = {});
    void addFragmentFromSmiles(const QString &initial = {});
    void addFragmentFromFile();
    void loadPartFile(const QString &path);
    void showReport();
    void compareDialog();
    void zMatrixDialog();
    void symmetryDialog();
  signals:
    void geometryChanged();
    void openRequested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    MainWindow &window;
    const EditorSession &session;
    QSettings &settings;
    Ids selected;
    QVector<Ids> monitors;
    bool refreshing = false;
    MoleculeView *view = nullptr;
    CoordinatesPanel *coordinates = nullptr;
    QWidget *propertyColumn = nullptr;
    QButtonGroup *toolGroup = nullptr; // The editing tools in the rail at the left of "1 分子".
    QComboBox *toolBondKind = nullptr; // In the option bar of the bond and place tools; mirrors bondKind.
    QButtonGroup *unitGroup = nullptr, *categoryGroup = nullptr; // Selection unit, palette category.
    QComboBox *elementBox = nullptr, *bondKind = nullptr;
    QLineEdit *search = nullptr;
    QListWidget *palette = nullptr, *residueList = nullptr, *pointList = nullptr;
    QLabel *pointEmpty = nullptr, *myPartsEmpty = nullptr; // What to do while the lists are empty.
    // The pages at the right of "1 分子"; segments above them switch the hidden tab bar.
    QTabWidget *properties = nullptr;
    QButtonGroup *propertyGroup = nullptr;
    ViewControls *viewControls = nullptr;
    // The cards of a page of properties by the form that adds to it, and the row of buttons that
    // button() adds to (its row in the form).
    QHash<QFormLayout *, QVBoxLayout *> pageCards;
    QHash<QFormLayout *, std::pair<int, QWidget *>> buttonRows;
    QLabel *measurement = nullptr, *sequenceLabel = nullptr, *mouseHint = nullptr, *measureTitle = nullptr,
           *dimensionLabel = nullptr, *selectionTitle = nullptr, *selectionList = nullptr,
           *candidateChanges = nullptr, *toolPart = nullptr;
    QWidget *chargeEditor = nullptr;
    // The charge page: the calculation's charge and point-charge settings ("2 計算条件"), shown.
    QLabel *chargeSummary = nullptr, *chargeState = nullptr, *pointChargeState = nullptr,
           *inputState = nullptr;
    QCheckBox *deleteTerminate = nullptr;      // "削除後に水素で終端化" in the edit panel,
    QCheckBox *toolDeleteTerminate = nullptr;  // and the same in the option bar of the delete tool.
    QDoubleSpinBox *toolChargeValue = nullptr; // The point-charge tool's charge; mirrors chargeValue.
    CandidateControls *banner = nullptr;
    // Above the 3D view: the options of the current tool, or the candidate in their place.
    QStackedWidget *topRow = nullptr;
    QSplitter *moleculeSplit = nullptr;         // The 3D view over the coordinate table.
    QComboBox *moleculeLayout = nullptr;        // "view", "both" or "table", behind the segments under them.
    SettingsCard *positionCard = nullptr;       // "座標（1原子）", hidden while the table is shown.
    QWidget *paletteColumn = nullptr;           // The palette of parts, folded with
    QToolButton *paletteToggle = nullptr;       // "パレット" at the bottom of the tool rail.
    QWidget *myPartsBar = nullptr;              // Saving, reading and removing the user's parts.
    QHash<QString, QWidget *> toolOptionGroups; // Option bar groups by object name.
    QWidget *sequenceChip = nullptr, *measureCard = nullptr, *candidateCard = nullptr;
    QAction *showHydrogens = nullptr, *showIndices = nullptr;
    QActionGroup *labelModes = nullptr;
    QCheckBox *autoH = nullptr, *keepSelection = nullptr, *keepTool = nullptr, *growth = nullptr;
    QPushButton *cancelWorkerButton = nullptr, *periodicTableButton = nullptr;
    QString paletteElement = "C";
    // A center element chosen outside the listed metals (S, P, Xe, ...) with the element buttons,
    // the periodic table or the search; listed first under "配位中心".
    QString centerElement;
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
    int ligandCandidate = 0; // The candidate serial of the ligand proposed last (proposeLigand).
    void buildEditPanel();
    void buildCoordinationPanel();
    void buildResiduePanel();
    void buildChargePanel();
    void refreshChargePanel();
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
    void applyMoleculeLayout();
    QString selectionUnit() const;
    QString paletteCategory() const;
    void setPaletteCategory(const QString &code);
    void showPalette(bool show);
    void refreshSelection();
    void refreshPalette();
    QString askSmiles(const QString &title, const QString &label, const QString &initial);
    void contextMenu(const QPoint &p, const QString &id);
    void savePart();
    void loadPart();
    void removePart();
    void selectNumbers();
    void deleteAtoms(const Ids &ids);
    // The edits that the tools, buttons, keys and menus share, with the options of the panel.
    void bondAtoms(const QString &a, const QString &b, const QString &kind);
    void replaceElements(const Ids &ids, const QString &symbol);
    void fixAtoms(const Ids &ids, bool fixed);
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
    void pdbPreparation();
    void addAmino(const QString &rid, bool n);

    const Document &document() const {
        return session.document();
    }
    const Document &displayDocument() const {
        return session.displayDocument();
    }
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview = false);
    void safe(const std::function<void()> &operation);
    QStatusBar *statusBar() const;
    bool chemistryRunning() const;
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {}, int count = 10);
    void cancelWorker();
    void confirmCandidate();
    void cancelCandidate();
    bool resolveCandidate();
    void checkMcpPath(const QString &path) const;
    void showSetting(int tab, const QString &setting = {});
    void showInputLine();
};
} // namespace mf
