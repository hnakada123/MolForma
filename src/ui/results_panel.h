// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_loader.h"
#include "results/result_presentation.h"
#include "results/result_summary.h"
#include "ui/editor_context.h"
#include <QCache>
#include <QTimer>
#include <QWidget>
#include <optional>
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
class SpectroscopyView;
class PropertyGeometryView;
class ResultEnergiesView;
class ResultOptimizationView;
class ResultExcitationsView;
class ResultPathView;
class ResultPropertiesView;
class ResultMeasurement;
class ElidedLabel;
class ViewControls;
class ToolbarFit;
class FrameTimeline;
class MoleculeView;
class OrbitalLevels;
struct ResultViewerState {
    QString pendingPath;
    ResultAnimation animation = ResultAnimation::None;
    int frame = -1, mode = -1;
    double amplitude = 0;
};
// The independent result viewer, the composer of its parts: it opens files and job folders, reads them
// with the cancellable ResultLoader, keeps the result with what is chosen of it (the structure, mode
// or orbital and the playback: results/result_presentation.h), owns the 3D view, the exports and the
// tabs bound to them (概要, 軌道, 振動, CUBE / 等値面, 元ファイル), and connects the tabs that only
// show the result: ResultEnergiesView, ResultOptimizationView, ResultExcitationsView,
// ResultPathView, ResultPropertiesView, PropertyGeometryView and SpectroscopyView. The measurement
// over the 3D view is ResultMeasurement, through which every structure goes into the view.
class ResultsPanel : public QWidget {
    Q_OBJECT
  public:
    ResultsPanel(EditorContext &editorContext, UiServices &uiContext, WorkspaceActions &workspace,
                 QWidget *parent = nullptr);
    ~ResultsPanel() override;
    void openFile(const QString &path);
    void openDirectory(const QString &path);
    // Writes the sum of CUBE files times their factors (a difference density) to output and shows it:
    // the files share one grid (combineCubes). Runs in the cancellable reader, like a load.
    void combineCubes(const QVector<CubeTerm> &terms, const QString &output);
    void cancelLoad();
    bool loading() const {
        return loader.active();
    }
    const ResultData &result() const {
        return data;
    }
    void saveGeometry(const QString &path) const;
    // Proposes the displayed result geometry as a candidate for the editor's molecule.
    void applyStructure();
    // Restores this job's saved conditions and proposes the displayed geometry for a new run; with a
    // task ("saddle_frequencies"), a run of that task.
    void prepareRecalculation(const QString &task = {});
    // One line with the outcome of the loaded log: optimization state, final energy, imaginary
    // frequencies and excited states (empty when the file has none of them).
    QString briefSummary() const;
    // The viewer's message: loading, an error or a comparison, else how the loaded file ended with
    // briefSummary() (shown in "概要"; the MCP results read it).
    QString message() const;
    // Style sheets in the editor's chrome colors; again when the palette changes.
    void applyChromeStyle();
    ResultViewerState viewerState() const;
    void reloadResult();
    void selectFrame(int index);
    void selectMode(int index, std::optional<double> amplitude = {}, bool play = false);
    void selectOrbital(int index);
    void playTrajectory();
    void stopAnimation();
    const Document &displayedGeometry() const;
  signals:
    void loaded();
    void loadFailed(QString message);
    void recalculationRequested(const QString &directory, const Document &geometry, const QString &task);

  private:
    EditorContext &editorContext;
    UiServices &uiContext;
    WorkspaceActions &workspace;
    ResultData data;
    ResultLoader loader;
    QTimer animation;
    ResultPresentation presentation;
    QString folder;
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
    QPushButton *cancel, *reload, *applyGeometry, *compareGeometry, *recalculate;
    QToolButton *openButton, *exportButton;
    QAction *saveXyz, *savePng, *saveFrames;
    // The 3D view with its overlays: what is shown, the view controls and the measurement.
    MoleculeView *view;
    QFrame *contextChip;
    ResultMeasurement *measurement;
    ViewControls *viewControlsFrame;
    QFrame *dockFrame; // Under the view: the structures, orbital or mode, and the hint.
    QString summary;   // How the loaded file ended, for message().
    ToolbarFit *barFit;
    QLabel *contextTitle, *contextDetail;
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
    QTableWidget *orbitals, *modes;
    ChartView *irSpectrum;
    // The tabs that show the loaded result and ask for what they need with signals.
    ResultEnergiesView *energiesView;
    ResultOptimizationView *optimizationView;
    ResultExcitationsView *excitationsView;
    ResultPathView *pathView;
    ResultPropertiesView *propertiesView;
    SpectroscopyView *spectroscopy;
    PropertyGeometryView *propertyGeometry;
    OrbitalLevels *levels;
    QDoubleSpinBox *irWidth;
    QLabel *orbitalStatus, *orbitalSource, *modeSummary;
    QWidget *orbitalOptions; // Isovalue, grid and normalization: only for a MOLDEN file.
    QLineEdit *isovalue, *orbitalIso;
    QPushButton *openMolden; // A log's orbitals: their shapes from the MOLDEN file of the same job.
    QPushButton *applyIso, *combineCube;
    int orbitalLoad = -1;              // Orbital of the shown file being read (-1: another load).
    QVector<ResultModeKind> modeKinds; // Of data.modes: vibrations, translations and rotations.
    QVector<int> irModes;              // The mode of each stick of the IR spectrum.
    // Energy of each frame relative to the lowest one / kcal mol⁻¹, where the result ties them
    // (scan and IRC points); NaN elsewhere.
    QVector<double> frameEnergies;
    // NWChem "freq animate" writes one XYZ per mode and step (<name>.m-001.s-001.xyz). The file
    // list shows one entry per mode; choosing it loads the steps joined into one trajectory.
    QMap<QString, QStringList> animationFrames; // "animation:<name>:<mode>" → step files
    bool animationSource = false;
    // The parts of the window, built once by the constructor in this order.
    QWidget *buildBar();
    QWidget *buildViewCard();
    QWidget *buildDock();
    QWidget *buildDetails();
    void buildTabs();
    QWidget *buildOrbitalTab();
    QWidget *buildVibrationTab();
    QWidget *buildSurfaceTab();
    QWidget *buildLogTab();
    void connectActions();
    void fillFiles(const QString &selected);
    void openEntry(const QString &entry);
    // The MOLDEN file of the job a log belongs to, as an entry of the file list (empty if none).
    QString jobMolden() const;
    void openOrbitalShape(int logOrbital);
    void startLoad(const QString &path, double iso, int channel, int orbital = -1,
                   const QString &normalization = "auto", int divisions = 40, bool reuseOrbital = false,
                   bool animation = false, bool reread = false, const QVector<CubeTerm> &combine = {});
    void showCombineDialog();
    void renderOrbital(int orbital);
    void cacheOrbital();
    void display(bool orbitalOnly = false);
    // Shows the detail tabs that have data in the loaded file (概要 always), with their counts.
    void updateTabs();
    void showOverview();
    void setOverview(QWidget *page);
    void showOutcome();
    void showPresentation();
    void showGeometry();
    // An orbital of a pair listed for the chosen excited state (its spin and NWChem's number): its
    // shape from the job's MOLDEN file, else its row in "軌道".
    void openTransitionOrbital(const QString &spin, int number);
    void showIrSpectrum();
    void showArrows();
    // The overlay naming what the view shows and the page of the controls under it.
    void showContext();
    void exportMode(bool images);
    // The 3D view as a picture (options: scale, background), and the structures (the points of a path in
    // its order, else every structure) or a vibration as a GIF, MP4 or numbered PNG files.
    void exportImage();
    void exportFrames();
    void updateActions();
    void error(const QString &message);
};
} // namespace mf
