#pragma once
#include "results/results.h"
#include <QCache>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QToolButton;
namespace mf {
class ChartView;
class ElidedLabel;
class ViewControls;
class FrameTimeline;
class MainWindow;
class MoleculeView;
class OrbitalLevels;
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
    // Style sheets in the editor's chrome colors; again when the palette changes.
    void applyChromeStyle();
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
    // An orbital of a log (spin, number) to show once the job's MOLDEN file has loaded.
    struct PendingOrbital {
        QString path, spin;
        int number = 0;
    } pendingOrbital;
    struct OrbitalSurface {
        QVector<SurfaceTriangle> triangles;
        std::array<int, 3> grid;
        double minimum, maximum;
        QString normalization;
        QStringList warnings;
    };
    QCache<QString, OrbitalSurface> orbitalCache{64 * 1024 * 1024};
    QString orbitalCacheSource;
    // The bar above: the job folder, the file, its kind and outcome, and the actions on it.
    QFrame *resultBar;
    QLabel *jobIcon, *emptyHint, *formatChip, *outcomeChip;
    ElidedLabel *jobLabel, *logName;
    QComboBox *files, *dataset, *orbitalNorm;
    QPushButton *cancel, *reload, *applyGeometry, *compareGeometry;
    QToolButton *openButton, *exportButton;
    QAction *saveXyz, *savePng;
    // The 3D view with its overlays: what is shown, the view controls and the measurement.
    MoleculeView *view;
    QFrame *contextChip, *measureCard;
    ViewControls *viewControlsFrame;
    QLabel *contextTitle, *contextDetail, *measurementLabel;
    QList<QWidget *> viewControls; // Camera, labels and measuring: only with a structure shown.
    // Under the view, for what it shows: the structure list, the orbitals or the vibration.
    QStackedWidget *dock;
    FrameTimeline *timeline;
    QSpinBox *frame, *orbitalGrid;
    QLabel *frameEnergy, *orbitalPosition, *modeLabel;
    QPushButton *firstFrame, *lastFrame, *playFrames, *playMode, *closeMode, *homo, *lumo;
    std::array<QPushButton *, 6> orbitalNavigation;
    QDoubleSpinBox *amplitude;
    QCheckBox *arrows, *showRigid;
    QToolButton *modeExport;
    QAction *saveModeImages, *saveModeTrajectory;
    // The details: an outcome banner over the tabs with data.
    QFrame *banner;
    QLabel *bannerIcon, *bannerTitle, *bannerText;
    QTabWidget *details;
    QScrollArea *overviewScroll;
    QPlainTextEdit *status, *log;
    QTableWidget *energy, *orbitals, *modes, *steps, *excitations, *path, *atomProperties;
    ChartView *energyChart, *gradientChart, *spectrum, *pathChart, *irSpectrum;
    OrbitalLevels *levels;
    QComboBox *spectrumAxis, *energyUnit;
    QDoubleSpinBox *broadening, *irWidth;
    QLabel *propertySummary, *energyNote, *orbitalStatus, *orbitalSource, *modeSummary;
    QFrame *criteria;
    QWidget *orbitalOptions; // Isovalue, grid and normalization: only for a MOLDEN file.
    QLineEdit *isovalue, *orbitalIso;
    QPushButton *openMolden; // A log's orbitals: their shapes from the MOLDEN file of the same job.
    QPushButton *energyReference, *applyIso;
    int referenceEnergy = -1;  // Row of data.energies that ΔE refers to (-1: the last energy).
    QVector<bool> rigidMotion; // Translations and rotations among data.modes.
    QVector<int> irModes;      // The mode of each stick of the IR spectrum.
    // Energy of each frame relative to the lowest one / kcal mol⁻¹, where the result ties them
    // (scan and IRC points); NaN elsewhere.
    QVector<double> frameEnergies;
    // NWChem "freq animate" writes one XYZ per mode and step (<name>.m-001.s-001.xyz). The file
    // list shows one entry per mode; choosing it loads the steps joined into one trajectory.
    QTemporaryDir animationDir;
    QMap<QString, QStringList> animationFrames; // "animation:<name>:<mode>" → step files
    QMap<QString, QString> animationSources;    // joined file → its list entry
    void fillFiles(const QString &selected);
    void openEntry(const QString &entry);
    // The MOLDEN file of the job a log belongs to, as an entry of the file list (empty if none).
    QString jobMolden() const;
    void openOrbitalShape(int logOrbital);
    // Measurement by atom number, so it follows frames and vibration snapshots (new atom IDs).
    QVector<int> picked;
    QVector<QVector<int>> pinned;
    void startLoad(const QString &path, double iso, int channel, int orbital = -1,
                   const QString &normalization = "auto", int divisions = 40, bool reuseOrbital = false);
    void renderOrbital(int orbital);
    void cacheOrbital();
    void display(bool orbitalOnly = false);
    // Shows the detail tabs that have data in the loaded file (概要 always), with their counts.
    void updateTabs();
    void showOverview();
    void setOverview(QWidget *page);
    void showOutcome();
    void showFrame();
    void showMode();
    void showSpectrum();
    void showEnergies();
    void showPath();
    void showProperties();
    void showIrSpectrum();
    void showOptimization();
    void showArrows();
    // The overlay naming what the view shows and the page of the controls under it.
    void showContext();
    void exportMode(bool images);
    void setViewDocument(const Document &document);
    void pickAtom(const QString &id);
    const Document &displayedGeometry() const;
    void stopAnimation();
    void updateActions();
    void error(const QString &message);
};
} // namespace mf
