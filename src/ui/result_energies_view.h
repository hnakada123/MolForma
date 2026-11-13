// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QWidget>
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
namespace mf {
// "エネルギー" of the result viewer: the energies of a result in the order of its output, each with its
// difference from a reference row (the last one until another is chosen) in the chosen unit.
class ResultEnergiesView : public QWidget {
  public:
    explicit ResultEnergiesView(QWidget *parent = nullptr);
    // Another result: the differences refer to its last energy again.
    void setResult(const ResultData &result);
    // "選択行をΔEの基準にする": for a result with energies, while none is being read.
    void setReferenceEnabled(bool enabled);

  private:
    QVector<ResultEnergy> energies;
    QComboBox *energyUnit;
    QPushButton *energyReference;
    QTableWidget *energy;
    QLabel *energyNote;
    int referenceEnergy = -1; // Row of energies that ΔE refers to (-1: the last energy).
    void showEnergies();
};
} // namespace mf
