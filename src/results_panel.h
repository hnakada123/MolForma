#pragma once
#include "results.h"
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
namespace mf {
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
  signals:
    void loaded();
    void loadFailed(QString message);

  private:
    MainWindow *window;
    ResultData data;
    QProcess *process = nullptr;
    std::unique_ptr<QTemporaryDir> work;
    QTimer animation;
    double phase = 0;
    bool modeDisplayed = false;
    QString folder, pendingPath;
    MoleculeView *view;
    QLabel *source, *status, *summary;
    QComboBox *files, *dataset;
    QSpinBox *frame;
    QDoubleSpinBox *amplitude;
    QLineEdit *isovalue;
    QPlainTextEdit *log;
    QTableWidget *energy, *orbitals, *modes;
    QPushButton *cancel, *reload, *playFrames, *playMode, *saveXyz, *savePng, *applyIso;
    void startLoad(const QString &path, double iso, int channel);
    void display();
    void showFrame();
    void showMode();
    void stopAnimation();
    void updateActions();
    void error(const QString &message);
};
} // namespace mf
