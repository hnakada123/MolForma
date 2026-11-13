// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_optimization_view.h"
#include "model/units.h"
#include "results/result_summary.h"
#include "ui/editor_context.h"
#include "ui/result_charts.h"
#include "ui/result_detail.h"
#include "ui/theme.h"
#include <QtWidgets>
#include <cmath>

namespace mf {
using namespace result_detail;
ResultOptimizationView::ResultOptimizationView(UiServices &uiContext, QWidget *parent) : QWidget(parent) {
    setObjectName("resultsOptimizationPage");
    auto *opl = new QVBoxLayout(this);
    criteria = new QFrame;
    criteria->setObjectName("resultsCriteria");
    auto *criteriaLayout = new QVBoxLayout(criteria);
    criteriaLayout->setContentsMargins(0, 0, 0, 0);
    criteriaLayout->setSpacing(6);
    opl->addWidget(criteria);
    auto *charts = new QHBoxLayout;
    energyChart = new ChartView;
    energyChart->setObjectName("resultsEnergyChart");
    charts->addWidget(energyChart, 1);
    gradientChart = new ChartView;
    gradientChart->setObjectName("resultsGradientChart");
    charts->addWidget(gradientChart, 1);
    opl->addLayout(charts, 1);
    steps = table(
        {tr("ステップ"), tr("エネルギー / Eh"), "ΔE / Eh", "Gmax", "Grms", "Xrms", "Xmax", tr("経過 / s")},
        "resultsSteps");
    steps->verticalHeader()->hide();
    opl->addWidget(steps, 1);
    auto *stepHint =
        new QLabel(tr("NWChem DRIVERの「@」行です。勾配はHartree/Bohr、変位はBohr。破線は収束閾値、"
                      "表の色付きの値は閾値を満たした値です。"));
    stepHint->setWordWrap(true);
    opl->addWidget(stepHint);
    for (auto *chart : {energyChart, gradientChart})
        setFigureExport(chart, uiContext, this);
}
void ResultOptimizationView::setResult(const ResultData &data, const QPalette &chrome) {
    const ChromeColors c(chrome);
    const auto thresholds = data.properties.value("thresholds").toObject();
    // The four criteria of the last step against their thresholds.
    auto *box = static_cast<QVBoxLayout *>(criteria->layout());
    while (auto *item = box->takeAt(0)) {
        if (item->layout())
            while (auto *inner = item->layout()->takeAt(0)) {
                delete inner->widget();
                delete inner;
            }
        delete item->widget();
        delete item;
    }
    criteria->setVisible(!data.steps.isEmpty());
    if (!data.steps.isEmpty()) {
        const auto &last = data.steps.last();
        auto *title = new QLabel(tr("収束判定（最終ステップ %1）").arg(last.step));
        auto font = title->font();
        font.setBold(true);
        title->setFont(font);
        box->addWidget(title);
        auto *row = new QHBoxLayout;
        row->setSpacing(6);
        for (const auto &criterion : resultCriteria(last, thresholds)) {
            const QString name = criterion.name;
            // Five decimals, as NWChem prints them.
            auto *chip = summaryTile(
                criterion.judged() ? QString("%1 %2").arg(name, criterion.met() ? "✓" : "✗") : name,
                criterion.measured ? fixed(criterion.value, 5) : QString("—"),
                std::isfinite(criterion.threshold) ? tr("閾値 %1").arg(fixed(criterion.threshold, 5))
                                                   : QString(),
                !criterion.judged() ? "neutral"
                : criterion.met()   ? "good"
                                    : "warn",
                false);
            chip->setObjectName("resultsCriterion" + name);
            row->addWidget(chip, 1);
        }
        box->addLayout(row);
    }
    steps->setRowCount(data.steps.size());
    QVector<QPointF> relative, gmax, grms;
    for (int i = 0; i < data.steps.size(); ++i) {
        const auto &t = data.steps[i];
        row(steps, i,
            {QString::number(t.step), fixed(t.energy, 8), real(t.deltaE), fixed(t.gmax, 5), fixed(t.grms, 5),
             fixed(t.xrms, 5), fixed(t.xmax, 5), real(t.seconds)});
        // Values within their threshold in the color of a met criterion (columns 3–6, in their order).
        const auto criteria = resultCriteria(t, thresholds);
        for (int k = 0; k < int(criteria.size()); ++k)
            if (criteria[k].met())
                steps->item(i, 3 + k)->setForeground(c.good);
        relative << QPointF(t.step, (t.energy - data.steps.last().energy) * HartreeToKcal);
        gmax << QPointF(t.step, t.gmax);
        grms << QPointF(t.step, t.grms);
    }
    if (data.steps.isEmpty()) {
        energyChart->clear();
        gradientChart->clear();
        return;
    }
    energyChart->setChart(tr("エネルギー（最終ステップ基準）"), tr("ステップ"), "ΔE / kcal mol⁻¹",
                          {{tr("エネルギー"), relative, ChartTone::Accent}});
    QVector<QPair<QString, double>> levels;
    if (thresholds.contains("gmax"))
        levels.append({tr("Gmax閾値"), thresholds["gmax"].toDouble()});
    if (thresholds.contains("grms"))
        levels.append({tr("Grms閾値"), thresholds["grms"].toDouble()});
    gradientChart->setChart(tr("勾配"), tr("ステップ"), "Hartree / Bohr",
                            {{"Gmax", gmax, ChartTone::Accent}, {"Grms", grms, ChartTone::Second}}, true,
                            levels);
}
} // namespace mf
