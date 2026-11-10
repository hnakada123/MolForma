// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/chemistry.h"
#include "results/result_loader.h"
#include "results/spectrum.h"
#include <QDialog>
#include <QPointer>
class QTableWidget;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
namespace mf {
class ChartView;
class MainWindow;
// The spectrum of a conformer set in fast exchange: the sticks of every conformer times its
// population, broadened once (averagedSpectrum), or for NMR the mean shielding of each atom.
class ConformerSpectrumDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ConformerSpectrumDialog(MainWindow *window, QWidget *parent);
    // [{name, weight, spectra}] of the conformers with a population.
    void setMembers(const QJsonArray &members);
    QJsonObject parameters() const;
    void setParameters(const QJsonObject &parameters);
    void exportCsv(const QString &path) const;

  private:
    MainWindow *window;
    QJsonArray members;
    SpectrumData spectrum;
    QComboBox *kind, *nucleus;
    QLineEdit *reference;
    QDoubleSpinBox *width;
    QLabel *nucleusLabel, *referenceLabel, *message;
    ChartView *chart;
    void refresh();
};
class ConformerSetDialog : public QDialog {
    Q_OBJECT
  public:
    using Enqueue = std::function<QStringList(const QVector<Document> &, bool)>;
    ConformerSetDialog(MainWindow *window, Enqueue enqueue);
    void setConformers(const Document &original, const QVector<Conformer> &conformers);
    QJsonObject specification() const;
    void setSpecification(const QJsonObject &specification);
    void applySelected();
    void queueSelected();
    void refreshResults();
    // The conformers with a population, for averagedSpectrum.
    QJsonArray spectrumMembers() const;
    // Opens the averaged spectrum of the populations shown; it follows them while open.
    ConformerSpectrumDialog *showSpectrum();

  private:
    MainWindow *window;
    Enqueue enqueue;
    Document original, pendingOriginal;
    QJsonArray entries;
    QVector<double> populations; // Of each entry; negative without one.
    ResultLoader loader;
    QVector<int> pending;
    int reading = -1;
    QTableWidget *table;
    QDoubleSpinBox *temperature, *tolerance, *frequencyScale, *entropyCutoff;
    QComboBox *metric, *entropyModel;
    QCheckBox *frequencies;
    QPushButton *spectrumButton;
    QLabel *message;
    QPointer<ConformerSpectrumDialog> spectrumDialog;
    void refresh();
    void readNext();
    Document geometry(int index) const;
    void checkMolecule() const;
};
} // namespace mf
