// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_loader.h"
#include <QDialog>
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QTableWidget;
namespace mf {
class ChartView;
class MainWindow;
class EnergyProfileDialog : public QDialog {
    Q_OBJECT
  public:
    explicit EnergyProfileDialog(MainWindow *window);
    void addJob(const QString &directory);
    QJsonObject specification() const;
    void setSpecification(const QJsonObject &specification);

  private:
    MainWindow *window;
    ResultLoader loader;
    Document pending;
    QJsonObject pendingSettings;
    QJsonArray records;
    QComboBox *metric, *unit, *reference, *entropyModel, *standardCorrection;
    QDoubleSpinBox *temperature, *frequencyScale, *entropyCutoff;
    QLineEdit *standard;
    QTableWidget *sources, *terms, *values;
    ChartView *chart;
    QLabel *message;
    void addTerm(const QString &stage, const QJsonObject &term);
    void rebuildSources();
    void updateProfile();
    void save(const QString &path) const;
    void load(const QString &path);
};
} // namespace mf
