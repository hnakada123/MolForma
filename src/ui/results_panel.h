#pragma once
#include "results/results.h"
#include <QCache>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
class QLabel;
class QPushButton;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QCheckBox;
namespace mf {
class ChartView;
class MainWindow;
class MoleculeView;
class ResultsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit ResultsPanel(MainWindow *owner);
    ~ResultsPanel() override;
    void openFile(const QString &path);
    void openDirectory(const QString &path);
    void cancelLoad();
    bool loading() const {
        return process != nullptr;
    }
    const ResultData &result() const {
        return data;
    }
    void saveGeometry(const QString &path) const;
    // Proposes the displayed result geometry as a candidate for the editor's molecule.
    void applyStructure();
    // One line with the outcome of the loaded log: optimization state, final energy, imaginary
    // frequencies and excited states (empty when the file has none of them).
    QString briefSummary() const;
  signals:
    void loaded();
    void loadFailed(QString message);

  private:
    friend class McpGui;
    MainWindow *window;
    ResultData data;
    QProcess *process = nullptr;
    std::unique_ptr<QTemporaryDir> work;
    QTimer animation;
    double phase = 0;
    bool modeDisplayed = false;
    QString folder, pendingPath;
    struct OrbitalSurface {
        QVector<SurfaceTriangle> triangles;
        std::array<int, 3> grid;
        double minimum, maximum;
        QString normalization;
        QStringList warnings;
    };
    QCache<QString, OrbitalSurface> orbitalCache{64 * 1024 * 1024};
    QString orbitalCacheSource;
    MoleculeView *view;
    QLabel *source, *summary;
    QComboBox *files, *dataset, *orbitalNorm;
    QSpinBox *frame, *orbitalGrid;
    QDoubleSpinBox *amplitude;
    QLineEdit *isovalue, *orbitalIso;
    QLabel *orbitalStatus;
    QLabel *orbitalPosition;
    std::array<QPushButton *, 6> orbitalNavigation;
    QTabWidget *details;
    QPlainTextEdit *status, *log;
    QTableWidget *energy, *orbitals, *modes, *steps, *excitations, *path, *atomProperties;
    ChartView *energyChart, *gradientChart, *spectrum, *pathChart, *irSpectrum;
    QComboBox *spectrumAxis, *energyUnit;
    QDoubleSpinBox *broadening, *irWidth;
    QLabel *propertySummary, *energyNote;
    QCheckBox *arrows, *showRigid;
    QPushButton *openMolden;       // A log's orbitals: open the MOLDEN file of the same job.
    QList<QWidget *> viewControls; // Camera, labels and measuring: only with a structure shown.
    // Shows the detail tabs that have data in the loaded file (概要 always).
    void updateTabs();
    QPushButton *energyReference, *saveModeImages, *saveModeTrajectory, *compareGeometry;
    int referenceEnergy = -1;  // Row of data.energies that ΔE refers to (-1: the last energy).
    QVector<bool> rigidMotion; // Translations and rotations among data.modes.
    // NWChem "freq animate" writes one XYZ per mode and step (<name>.m-001.s-001.xyz). The file
    // list shows one entry per mode; choosing it loads the steps joined into one trajectory.
    QTemporaryDir animationDir;
    QMap<QString, QStringList> animationFrames; // "animation:<name>:<mode>" → step files
    QMap<QString, QString> animationSources;    // joined file → its list entry
    void fillFiles(const QString &selected);
    void openEntry(const QString &entry);
    // Measurement by atom number, so it follows frames and vibration snapshots (new atom IDs).
    QVector<int> picked;
    QVector<QVector<int>> pinned;
    QLabel *measurementLabel;
    QPushButton *cancel, *reload, *playFrames, *playMode, *saveXyz, *savePng, *applyIso, *applyGeometry;
    QPushButton *homo, *lumo;
    void startLoad(const QString &path, double iso, int channel, int orbital = -1,
                   const QString &normalization = "auto", int divisions = 40, bool reuseOrbital = false);
    void renderOrbital(int orbital);
    void cacheOrbital();
    void display(bool orbitalOnly = false);
    void showFrame();
    void showMode();
    void showSpectrum();
    void showEnergies();
    void showPath();
    void showProperties();
    void showIrSpectrum();
    void showArrows();
    void exportMode(bool images);
    void setViewDocument(const Document &document);
    void pickAtom(const QString &id);
    const Document &displayedGeometry() const;
    void stopAnimation();
    void updateActions();
    void error(const QString &message);
};
} // namespace mf
