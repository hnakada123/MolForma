// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_path_view.h"
#include "foundation/i18n.h"
#include "model/units.h"
#include "ui/editor_context.h"
#include "ui/line_icons.h"
#include "ui/result_charts.h"
#include "ui/result_detail.h"
#include <QtWidgets>

namespace mf {
using namespace result_detail;
namespace {
// "bond 1 2" (MolForma's scan title) in the words of the calculation tab.
QString coordinateText(const QString &coordinate) {
    const auto words = coordinate.split(' ', Qt::SkipEmptyParts);
    const auto atoms = words.mid(1).join("–");
    return words.value(0) == "bond"    ? tr("距離 %1").arg(atoms)
           : words.value(0) == "angle" ? tr("角度 %1").arg(atoms)
                                       : tr("二面角 %1").arg(atoms);
}
} // namespace
ResultPathView::ResultPathView(UiServices &uiContext, QWidget *parent) : QWidget(parent) {
    auto *pl = new QVBoxLayout(this);
    path = table({tr("点"), tr("座標・方向"), tr("値"), tr("エネルギー / Eh"), "ΔE / kcal mol⁻¹"},
                 "resultsPath");
    pl->addWidget(path, 1);
    pathChart = new ChartView;
    pathChart->setObjectName("resultsPathChart");
    pl->addWidget(pathChart, 1);
    pathMap = new ScanMap;
    pathMap->setObjectName("resultsPathMap");
    pathMap->hide();
    pl->addWidget(pathMap, 1);
    connect(pathMap, &ScanMap::pointClicked, this, [this](int row) { path->setCurrentCell(row, 0); });
    setFigureExport(pathChart, uiContext, this);
    auto *pathActions = new QHBoxLayout;
    pathNote = new QLabel;
    pathNote->setObjectName("resultsPathNote");
    pathNote->setWordWrap(true);
    pathActions->addWidget(pathNote, 1);
    pathHighest = new QPushButton(tr("最高点を選択"));
    pathHighest->setObjectName("resultsPathHighest");
    pathHighest->setToolTip(tr("経路の両端を除いて、エネルギーが最も高い点を選びます。"));
    pathActions->addWidget(pathHighest);
    pathSaddle = new QPushButton(lineIcon("refresh"), tr("この点から遷移状態探索を準備"));
    pathSaddle->setObjectName("resultsPathSaddle");
    pathSaddle->setToolTip(tr("このジョブの計算条件を読み込み、計算目的を「遷移状態探索 → 振動数」にして、"
                              "表示中の点の構造を候補にします。確定後、新しいジョブとして実行できます。"));
    pathActions->addWidget(pathSaddle);
    pl->addLayout(pathActions);
    connect(pathHighest, &QPushButton::clicked, this, [this] {
        // The highest point between the ends; the table lists the points in their order.
        if (const int best = shownPath.highest(PathEnds::Excluded); best >= 0)
            path->setCurrentCell(best, 0);
    });
    connect(pathSaddle, &QPushButton::clicked, this, &ResultPathView::saddleSearchRequested);
    auto *pathHint =
        new QLabel(tr("座標スキャン: 各点の最後のエネルギー（緩和スキャンは最適化後）で、ΔEは最小値基準です。"
                      "IRC: 遷移状態を0とし、前進方向を正・後退方向を負の反応座標（amu½·Bohr）で表示し、"
                      "ΔEは遷移状態基準です。NEB: 最後に出力された経路の像を、始点からの経路の長さ（Å）で"
                      "表示し、ΔEは始点基準です。行を選ぶとその構造を表示します。"));
    pathHint->setWordWrap(true);
    pl->addWidget(pathHint);
    connect(path, &QTableWidget::currentCellChanged, this, [this](int row) {
        pathMap->setSelected(row);
        if (row < 0 || row >= shownPath.points.size())
            return;
        const int index = shownPath.points[row].frame;
        if (index >= 0 && index < frames)
            emit frameRequested(index);
    });
}
void ResultPathView::setSaddleEnabled(bool enabled) {
    pathSaddle->setEnabled(enabled);
}
void ResultPathView::setResult(const ResultData &data) {
    using Role = ResultPathPoint::Role;
    const QSignalBlocker guard(path);
    path->setRowCount(0);
    shownPath = resultPath(data);
    frames = data.frames.size();
    const auto &points = shownPath.points;
    const auto kind = shownPath.kind;
    const bool plane = shownPath.plane;
    const auto neb = data.properties.value("neb").toObject(); // The state of the NEB, for the note.
    QString xLabel, yLabel;
    if (kind == ResultPathKind::Scan) {
        const auto axis = [](const QString &coordinate) {
            return coordinateText(coordinate) + (coordinate.startsWith("bond") ? " / Å" : " / °");
        };
        const auto &first = points.first();
        xLabel = plane                                 ? axis(first.coordinate)
                 : first.coordinate.startsWith("bond") ? tr("距離 / Å")
                                                       : tr("角度 / °");
        yLabel = plane ? axis(first.coordinate2) : QString();
    } else if (kind == ResultPathKind::Neb)
        xLabel = shownPath.lengths ? tr("経路の長さ / Å") : tr("像");
    else if (kind == ResultPathKind::Irc)
        xLabel = tr("反応座標 s / amu½·Bohr");
    // The first two columns: the point in the words of its path.
    const auto name = [&](const ResultPathPoint &p) {
        return p.role == Role::Forward           ? tr("前進 %1").arg(p.number)
               : p.role == Role::Backward        ? tr("後退 %1").arg(p.number)
               : p.role == Role::TransitionState ? QString("TS")
                                                 : QString::number(p.number);
    };
    const auto place = [&](const ResultPathPoint &p) {
        if (kind == ResultPathKind::Scan)
            return plane ? tr("%1 = %2、%3 = %4")
                               .arg(coordinateText(p.coordinate), fixed(p.x, 4),
                                    coordinateText(p.coordinate2), fixed(p.y, 4))
                         : coordinateText(p.coordinate);
        return p.role == Role::Start             ? tr("始点")
               : p.role == Role::End             ? tr("終点")
               : p.role == Role::Forward         ? tr("前進")
               : p.role == Role::Backward        ? tr("後退")
               : p.role == Role::TransitionState ? tr("遷移状態")
                                                 : QString();
    };
    path->setRowCount(points.size());
    QVector<QPointF> curve;
    QVector<ScanMap::Point> map;
    for (int i = 0; i < points.size(); ++i) {
        const auto &p = points[i];
        const double relative = shownPath.relative(i) * HartreeToKcal;
        row(path, i,
            {name(p), place(p), plane ? fixed(p.x, 4) + ", " + fixed(p.y, 4) : fixed(p.x, 4),
             fixed(p.energy, 8), fixed(relative, 3)});
        // For readers of the table from outside (the widget snapshots of MCP): the view itself takes
        // frames and energies from shownPath.
        path->item(i, 0)->setData(Qt::UserRole, p.frame);
        path->item(i, 3)->setData(Qt::UserRole, p.energy);
        curve << QPointF(p.x, relative);
        map << ScanMap::Point{p.x, p.y, relative, i};
    }
    pathChart->setVisible(!plane);
    pathMap->setVisible(plane);
    if (plane)
        pathMap->setMap(tr("2次元スキャン（最小値基準）"), xLabel, yLabel, "ΔE / kcal mol⁻¹", map);
    else
        pathMap->clear();
    if (curve.isEmpty())
        pathChart->clear();
    else
        pathChart->setChart(kind == ResultPathKind::Scan  ? tr("座標スキャン（最小値基準）")
                            : kind == ResultPathKind::Neb ? tr("NEB（始点基準）")
                                                          : tr("IRC（遷移状態基準）"),
                            xLabel, "ΔE / kcal mol⁻¹", {{tr("エネルギー"), curve, ChartTone::Accent}});
    QString note;
    if (!neb.isEmpty()) {
        const auto steps = neb["steps"].toArray();
        const auto last = steps.isEmpty() ? QJsonObject() : steps.last().toObject();
        const auto thresholds = neb["thresholds"].toObject();
        note = tr("ステップ %1 の経路").arg(neb["iteration"].toInt());
        if (neb.contains("converged"))
            note += neb["converged"].toBool() ? tr("（収束）") : tr("（未収束）");
        if (!last.isEmpty() && thresholds.contains("gmax"))
            note += tr("。最後のステップのGmax %1（閾値 %2）、Grms %3（閾値 %4）")
                        .arg(shownReal(last["gmax"].toDouble()), shownReal(thresholds["gmax"].toDouble()),
                             shownReal(last["grms"].toDouble()), shownReal(thresholds["grms"].toDouble()));
        note += tr("。最高点の構造から遷移状態探索へ進めます。");
    }
    pathNote->setText(note);
    pathNote->setVisible(!note.isEmpty());
    pathHighest->setEnabled(points.size() >= 3);
}
} // namespace mf
