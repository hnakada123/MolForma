// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QWidget>
class QFrame;
class QPalette;
class QTableWidget;
namespace mf {
class ChartView;
class UiServices;
// "最適化" of the result viewer: the steps of the last optimization of a result, the criteria of its last
// step against their thresholds, and the energy and gradient of the steps as charts.
class ResultOptimizationView : public QWidget {
  public:
    explicit ResultOptimizationView(UiServices &uiContext, QWidget *parent = nullptr);
    // Values within their threshold take the color of a met criterion from `chrome`, the palette of
    // the viewer: shown again when that changes.
    void setResult(const ResultData &result, const QPalette &chrome);

  private:
    QFrame *criteria;
    ChartView *energyChart, *gradientChart;
    QTableWidget *steps;
};
} // namespace mf
