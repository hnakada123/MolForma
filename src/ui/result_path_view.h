// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_path.h"
#include <QWidget>
class QLabel;
class QPushButton;
class QTableWidget;
namespace mf {
class ChartView;
class ScanMap;
class UiServices;
// "経路・スキャン" of the result viewer: the points of a coordinate scan, the images of an NEB or the
// points of an IRC (results/result_path.h) as a table with their energies, and as a chart or, for a
// two-dimensional scan, a map. What a chosen point shows is the viewer's: the view asks for its structure.
class ResultPathView : public QWidget {
    Q_OBJECT
  public:
    explicit ResultPathView(UiServices &uiContext, QWidget *parent = nullptr);
    void setResult(const ResultData &result);
    // The path listed in the table, a row for each of its points.
    const ResultPath &shown() const {
        return shownPath;
    }
    // "この点から遷移状態探索を準備": for the result of a job whose conditions can be read.
    void setSaddleEnabled(bool enabled);
  signals:
    // A point was chosen (a row, the map, "最高点を選択"): its structure, an index of the result's frames.
    void frameRequested(int frame);
    // A transition state search from the structure shown.
    void saddleSearchRequested();

  private:
    QTableWidget *path;
    QPushButton *pathHighest, *pathSaddle; // Under the path: its highest point, and a saddle search there.
    QLabel *pathNote;
    ResultPath shownPath;
    int frames = 0; // The structures of the result.
    ChartView *pathChart;
    ScanMap *pathMap; // A two-dimensional scan, instead of the path chart.
};
} // namespace mf
