// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/spectrum.h"
#include "ui/editor_context.h"
#include <QWidget>
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
class QCheckBox;
class QFormLayout;
namespace mf {
class ChartView;
class SpectroscopyView : public QWidget {
    Q_OBJECT
  public:
    SpectroscopyView(UiServices &uiContext, QWidget *parent = nullptr);
    void setResult(const ResultData &result);
    void selectAtom(int index);
    QJsonObject parameters() const;
    void setParameters(const QJsonObject &parameters);
    void saveSettings(const QString &path) const;
    void loadSettings(const QString &path);
    void exportCsv(const QString &path) const;
  signals:
    void atomSelected(int index);
    void modeSelected(int index);
    void excitationSelected(int index);

  private:
    UiServices &uiContext;
    QFormLayout *form;
    ResultData result;
    SpectrumData spectrum;
    QString stamp;
    QJsonObject weights;
    QComboBox *kind, *nucleus;
    QLineEdit *reference, *minimum, *maximum;
    QDoubleSpinBox *width;
    QDoubleSpinBox *microwave, *spectrometer;
    QCheckBox *derivative, *coupling;
    int peakOfStick(int stick) const;
    int firstStick(int peak) const;
    QLabel *message;
    ChartView *chart;
    QTableWidget *peaks, *nuclei;
    void refresh();
};
} // namespace mf
