#pragma once
#include "molecule_view.h"
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
namespace mf {
class CoordinatesPanel;
class CalculationPanel;
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

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    friend class DocumentCommand;
    friend class CoordinatesPanel;
    friend class CalculationPanel;
    Document doc;
    std::optional<Document> candidate;
    QString candidateName, path;
    Ids selected;
    QUndoStack undo;
    QSettings settings;
    QProcess *worker = nullptr;
    std::unique_ptr<QTemporaryDir> workdir;
    QByteArray workerBase;
    bool refreshing = false;
    MoleculeView *view = nullptr;
    CoordinatesPanel *coordinates = nullptr;
    CalculationPanel *calculation = nullptr;
    QTabWidget *workspaceTabs = nullptr;
    QComboBox *tool = nullptr, *unitBox = nullptr, *category = nullptr, *elementBox = nullptr,
              *bondKind = nullptr, *colorBox = nullptr;
    QLineEdit *search = nullptr;
    QListWidget *palette = nullptr, *residueList = nullptr, *pointList = nullptr;
    QTabWidget *properties = nullptr, *editPages = nullptr;
    QLabel *summary = nullptr, *measurement = nullptr, *candidateLabel = nullptr, *sequenceLabel = nullptr;
    QWidget *candidateBar = nullptr;
    QCheckBox *autoH = nullptr, *keepSelection = nullptr, *keepTool = nullptr, *showH = nullptr,
              *growth = nullptr;
    QPushButton *cancelWorkerButton = nullptr;
    QDoubleSpinBox *dimension = nullptr;
    QJsonArray customParts;
    QDoubleSpinBox *positionFields[3]{}, *translateFields[3]{}, *duplicateFields[3]{}, *chargeFields[3]{};
    QDoubleSpinBox *twist = nullptr, *distance = nullptr, *chargeValue = nullptr;
    QSpinBox *formal = nullptr, *radical = nullptr, *oxidation = nullptr, *totalCharge = nullptr,
             *multiplicity = nullptr;
    QLineEdit *mass = nullptr, *siteInput = nullptr, *sequenceInput = nullptr, *fragmentMembers = nullptr,
              *fragmentCharges = nullptr, *fragmentMult = nullptr, *fragmentCuts = nullptr;
    QComboBox *geometryBox = nullptr, *aminoState = nullptr, *chiralityBox = nullptr, *nTerm = nullptr,
              *cTerm = nullptr, *fragmentTarget = nullptr, *fragmentMode = nullptr;
    QString growthResidue;
    bool growthN = false;
    void buildUi();
    void buildMenus();
    void buildEditPanel();
    void buildCoordinationPanel();
    void buildResiduePanel();
    void buildFragmentPanel();
    void buildChargePanel();
    void refresh();
    void refreshPalette();
    void refreshSelection();
    void refreshFragmentFields();
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
    void setState(const Document &d);
    void commit(const QString &name, const Document &next);
    void newDocument();
    void open();
    bool save(bool as = false);
    bool resolveCandidate();
    bool confirmDiscard();
    void exportFile();
    void importSmiles();
    void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {});
    void cancelWorker();
    void contextMenu(const QPoint &p, const QString &id);
    void coordinateDialog();
    void savePart();
    void loadPart();
    void selectNumbers();
    void showReport();
    void pdbPreparation();
    void addAmino(const QString &rid, bool n);
};
} // namespace mf
