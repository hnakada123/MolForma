// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_excitations_view.h"
#include "model/units.h"
#include "results/result_summary.h"
#include "results/spectrum.h"
#include "ui/editor_context.h"
#include "ui/result_charts.h"
#include "ui/result_detail.h"
#include <QtWidgets>
#include <cmath>

namespace mf {
using namespace result_detail;
ResultExcitationsView::ResultExcitationsView(UiServices &uiContext, std::function<QString()> molden,
                                             QWidget *parent)
    : QWidget(parent), jobMolden(std::move(molden)) {
    auto *el = new QVBoxLayout(this);
    // Short headers where a language needs them, so that the oscillator strengths stay in view.
    excitations = table({tr("状態"), tr("スピン"), tr("対称", "an excited-state column"),
                         tr("励起エネルギー / eV", "an excited-state column"),
                         tr("波長 / nm", "an excited-state column"),
                         tr("振動子強度", "an excited-state column"), "<S²>", tr("主な遷移")},
                        "resultsExcitations");
    el->addWidget(excitations, 3);
    // The orbital pairs of the chosen state: only for a log that lists them. A few rows, so that the
    // states and the spectrum keep their room; a state with more pairs scrolls.
    contributionsBlock = new QWidget;
    auto *cl = new QVBoxLayout(contributionsBlock);
    cl->setContentsMargins(0, 0, 0, 0);
    contributionsTitle = new QLabel;
    contributionsTitle->setObjectName("resultsContributionsTitle");
    contributionsTitle->setWordWrap(true);
    cl->addWidget(contributionsTitle);
    contributions =
        table({tr("占有軌道"), tr("仮想軌道"), tr("遷移", "an orbital pair"), "X", "Y", tr("重み / %")},
              "resultsContributions");
    contributions->verticalHeader()->hide();
    contributions->setMaximumHeight(contributions->horizontalHeader()->sizeHint().height() +
                                    5 * contributions->verticalHeader()->defaultSectionSize());
    cl->addWidget(contributions, 1);
    el->addWidget(contributionsBlock);
    connect(excitations, &QTableWidget::currentCellChanged, this, &ResultExcitationsView::showContributions);
    connect(contributions, &QTableWidget::cellDoubleClicked, this,
            &ResultExcitationsView::openTransitionOrbital);
    auto *spectrumRow = new QHBoxLayout;
    el->addLayout(spectrumRow);
    spectrumAxis = new QComboBox;
    spectrumAxis->setObjectName("resultsSpectrumAxis");
    spectrumAxis->addItem(tr("横軸: 波長 / nm"), "nm");
    spectrumAxis->addItem(tr("横軸: 励起エネルギー / eV"), "ev");
    spectrumRow->addWidget(spectrumAxis);
    broadening = new QDoubleSpinBox;
    broadening->setObjectName("resultsBroadening");
    broadening->setRange(.05, 2);
    broadening->setSingleStep(.05);
    broadening->setValue(.3);
    broadening->setPrefix(tr("半値全幅 "));
    broadening->setSuffix(" eV");
    spectrumRow->addWidget(broadening);
    spectrumRow->addStretch();
    spectrum = new ChartView;
    spectrum->setObjectName("resultsSpectrum");
    // High enough for the name of its vertical axis beside the list of orbital pairs above it.
    spectrum->setMinimumHeight(200);
    el->addWidget(spectrum, 3);
    auto *spectrumHint =
        new QLabel(tr("各状態の振動子強度（棒）に、エネルギー軸で指定した半値全幅のガウス関数を掛けて重ね"
                      "た表示用の曲線です。三重項など振動子強度0の状態は棒に現れません。"));
    spectrumHint->setWordWrap(true);
    el->addWidget(spectrumHint);
    connect(spectrumAxis, &QComboBox::currentIndexChanged, this, &ResultExcitationsView::showSpectrum);
    connect(broadening, &QDoubleSpinBox::valueChanged, this, &ResultExcitationsView::showSpectrum);
    setFigureExport(spectrum, uiContext, this);
}
void ResultExcitationsView::setResult(const ResultData &result) {
    data = result;
    {
        const QSignalBlocker guard(excitations);
        excitations->setRowCount(data.excitations.size());
        bool listed = false; // The orbital pairs of the states, in a log that prints them.
        bool spinSquares = false;
        QSet<QString> symmetries;
        for (int i = 0; i < data.excitations.size(); ++i) {
            const auto &x = data.excitations[i];
            spinSquares |= std::isfinite(x.s2);
            symmetries << x.symmetry.trimmed().toLower();
            // The heaviest orbital pair, with its share of the pairs NWChem printed.
            const auto pairs = resultTransitions(x);
            listed |= !pairs.isEmpty();
            const auto main =
                pairs.isEmpty() ? QString("—")
                : std::isfinite(pairs[0].weight)
                    ? tr("%1（%2%）").arg(resultTransitionText(data, pairs[0]), fixed(pairs[0].weight, 0))
                    : resultTransitionText(data, pairs[0]);
            row(excitations, i,
                {QString::number(x.root),
                 x.spin.isEmpty()      ? tr("非制限")
                 : x.spin == "singlet" ? tr("一重項")
                                       : tr("三重項"),
                 x.symmetry, fixed(x.ev, 4), fixed(EvNanometer / x.ev, 1), fixed(x.oscillator, 5),
                 std::isfinite(x.s2) ? fixed(x.s2, 4) : "—", main});
            excitations->item(i, 7)->setToolTip(main); // In full where the column is narrow.
        }
        // Columns that would say the same on every row stay hidden (no symmetry labels, restricted
        // states without <S²>) and so do the row numbers, which leaves the room to the transitions.
        excitations->setColumnHidden(2, listed && symmetries.size() <= 1);
        excitations->setColumnHidden(6, listed && !spinSquares);
        excitations->verticalHeader()->setVisible(!listed);
        excitations->setColumnHidden(7, !listed);
        contributionsBlock->setVisible(listed);
        // The first state is chosen, so that its orbital pairs are listed at once.
        excitations->clearSelection();
        excitations->setCurrentCell(data.excitations.isEmpty() ? -1 : 0, 0);
    }
    showContributions();
    showSpectrum();
}
void ResultExcitationsView::selectState(int index) {
    if (index >= 0 && index < excitations->rowCount())
        excitations->selectRow(index);
}
void ResultExcitationsView::showSpectrum() {
    const bool nm = spectrumAxis->currentData() == "nm";
    try {
        // The viewer owns its axis limits; the shared model owns peaks and broadening.
        const double width = broadening->value();
        double low = INFINITY, high = -INFINITY;
        for (const auto &state : data.excitations)
            if (state.ev > 0) {
                low = std::min(low, state.ev);
                high = std::max(high, state.ev);
            }
        auto values = calculateSpectrum(data, {{"kind", "uv"},
                                               {"width", width},
                                               {"minimum", std::max(.5, low - 3 * width)},
                                               {"maximum", high + 3 * width}});
        if (nm)
            for (auto *points : {&values.sticks, &values.curve})
                for (auto &point : *points)
                    point.setX(EvNanometer / point.x());
        spectrum->setToolTip({});
        spectrum->setChart(tr("吸収スペクトル（表示用）"), nm ? tr("波長 / nm") : tr("励起エネルギー / eV"),
                           tr("振動子強度"),
                           {{tr("広がり"), values.curve, ChartTone::Accent},
                            {tr("各状態"), values.sticks, ChartTone::Contrast, true}});
    } catch (const std::exception &e) {
        spectrum->clear();
        spectrum->setToolTip(QString::fromUtf8(e.what()));
    }
}
void ResultExcitationsView::showContributions() {
    const int state = excitations->currentRow();
    const auto pairs = state >= 0 && state < data.excitations.size()
                           ? resultTransitions(data.excitations[state])
                           : QVector<ResultTransition>();
    bool spins = false, deexcitations = false;
    for (const auto &x : data.excitations)
        for (const auto &c : x.contributions) {
            spins |= !c.spin.isEmpty();
            deexcitations |= c.deexcitation;
        }
    contributions->setRowCount(pairs.size());
    for (int i = 0; i < pairs.size(); ++i) {
        const auto &p = pairs[i];
        row(contributions, i,
            {QString::number(p.from), QString::number(p.to), resultTransitionText(data, p), fixed(p.x, 5),
             fixed(p.y, 5), fixed(p.weight, 1)});
    }
    // Columns that say nothing stay hidden: the pair again in numbers (restricted states whose
    // frontier orbitals are not known), and Y without such amplitudes (Tamm–Dancoff).
    contributions->setColumnHidden(2, !spins && resultHomoNumber(data, {}) <= 0);
    contributions->setColumnHidden(4, !deexcitations);
    contributions->clearSelection();
    contributions->setCurrentCell(-1, -1);
    const auto molden = jobMolden();
    QString text = state < 0 || state >= data.excitations.size()
                       ? tr("状態を選ぶと、その状態への軌道の寄与を表示します。")
                   : pairs.isEmpty()
                       ? tr("状態 %1 への軌道の寄与はログにありません。").arg(data.excitations[state].root)
                       : tr("状態 %1 への軌道の寄与（NWChemが出力する、係数が0.05を超える占有→仮想軌道の"
                            "対）。重みは対ごとの X² − Y² が、出力された対の合計に占める割合です。")
                             .arg(data.excitations[state].root);
    if (!pairs.isEmpty() && !molden.isEmpty())
        text +=
            '\n' +
            tr("軌道番号をダブルクリックすると、同じジョブの %1 からその軌道の形を表示します。").arg(molden);
    contributionsTitle->setText(text);
}
void ResultExcitationsView::openTransitionOrbital(int row, int column) {
    const int state = excitations->currentRow();
    if (state < 0 || state >= data.excitations.size())
        return;
    const auto pairs = resultTransitions(data.excitations[state]);
    if (row < 0 || row >= pairs.size())
        return;
    // The virtual orbital from its column, the occupied one from the others.
    const int number = column == 1 ? pairs[row].to : pairs[row].from;
    const QString spin = pairs[row].spin == "beta" ? "Beta" : "Alpha";
    emit orbitalRequested(spin, number);
}
} // namespace mf
