// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QWidget>
class QCheckBox;
class QLabel;
class QTableWidget;
namespace mf {
// Summary lines for BSSE, solvation, polarizability and the paths of a result (shown in "概要" and
// "物性" of the result viewer).
QString resultPropertyLines(const ResultData &data);
// "物性" of the result viewer: the properties of a result as text, the charges and NMR shieldings of
// its atoms, the bond indices of a Mulliken analysis and the NMR spin–spin couplings.
class ResultPropertiesView : public QWidget {
    Q_OBJECT
  public:
    explicit ResultPropertiesView(QWidget *parent = nullptr);
    void setResult(const ResultData &result);
  signals:
    // A bond index or a coupling was chosen: its two atoms (from 0) in the last structure of the result.
    void atomPairSelected(int a, int b);

  private:
    ResultData data;
    QLabel *propertySummary;
    QTableWidget *atomProperties;
    // The bond indices of a Mulliken analysis, and the NMR spin–spin couplings.
    QTableWidget *bondIndices, *couplings;
    QWidget *bondBlock, *couplingBlock;
    QLabel *bondTitle, *couplingTitle;
    QCheckBox *allBondIndices; // Also the pairs below NotableBondIndex.
    void showBondIndices();
};
} // namespace mf
