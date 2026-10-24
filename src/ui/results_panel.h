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
    QTableWidget *energy, *orbitals, *modes, *steps, *excitations;
    ChartView *energyChart, *gradientChart, *spectrum;
    QComboBox *spectrumAxis;
    QDoubleSpinBox *broadening;
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
    const Document &displayedGeometry() const;
    void stopAnimation();
    void updateActions();
    void error(const QString &message);
};
} // namespace mf
