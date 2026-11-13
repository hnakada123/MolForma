// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_energies_view.h"
#include "model/units.h"
#include "ui/result_detail.h"
#include <QtWidgets>

namespace mf {
using namespace result_detail;
ResultEnergiesView::ResultEnergiesView(QWidget *parent) : QWidget(parent) {
    auto *enl = new QVBoxLayout(this);
    auto *energyRow = new QHBoxLayout;
    enl->addLayout(energyRow);
    energyUnit = new QComboBox;
    energyUnit->setObjectName("resultsEnergyUnit");
    for (const auto &[label, unit] : {std::pair{"ΔE: kcal/mol", "kcal"},
                                      {"ΔE: kJ/mol", "kj"},
                                      {"ΔE: eV", "ev"},
                                      {"ΔE: Hartree", "hartree"},
                                      {"ΔE: cm⁻¹", "wavenumber"}})
        energyUnit->addItem(label, unit);
    energyRow->addWidget(energyUnit);
    energyReference = new QPushButton(tr("選択行をΔEの基準にする"));
    energyReference->setObjectName("resultsEnergyReference");
    energyRow->addWidget(energyReference);
    energyRow->addStretch();
    energy = table({tr("手法"), tr("全エネルギー / Eh"), "ΔE", tr("ログ行")}, "resultsEnergy");
    enl->addWidget(energy, 1);
    energyNote = new QLabel;
    energyNote->setObjectName("resultsEnergyNote");
    energyNote->setWordWrap(true);
    enl->addWidget(energyNote);
    connect(energyUnit, &QComboBox::currentIndexChanged, this, &ResultEnergiesView::showEnergies);
    connect(energyReference, &QPushButton::clicked, this, [this] {
        if (energy->currentRow() >= 0 && energy->currentRow() < energies.size()) {
            referenceEnergy = energy->currentRow();
            showEnergies();
        }
    });
}
void ResultEnergiesView::setResult(const ResultData &result) {
    energies = result.energies;
    referenceEnergy = -1;
    showEnergies();
}
void ResultEnergiesView::setReferenceEnabled(bool enabled) {
    energyReference->setEnabled(enabled);
}
void ResultEnergiesView::showEnergies() {
    const auto unit = energyUnit->currentData().toString();
    const double factor = unit == "kcal"         ? HartreeToKcal
                          : unit == "kj"         ? HartreeToKj
                          : unit == "ev"         ? HartreeToEv
                          : unit == "wavenumber" ? HartreeToWavenumber
                                                 : 1.;
    const QString label = energyUnit->currentText().section(':', 1).trimmed();
    energy->setHorizontalHeaderItem(2, new QTableWidgetItem("ΔE / " + label));
    const int reference = referenceEnergy >= 0 && referenceEnergy < energies.size()
                              ? referenceEnergy
                              : int(energies.size()) - 1;
    const QSignalBlocker guard(energy);
    energy->setRowCount(energies.size());
    for (int i = 0; i < energies.size(); ++i) {
        const auto &e = energies[i];
        row(energy, i,
            {e.method, fixed(e.hartree, 10),
             fixed((e.hartree - energies[reference].hartree) * factor, unit == "hartree" ? 8 : 3),
             e.line ? QString::number(e.line) : "—"});
        if (i == reference)
            for (int c = 0; c < 4; ++c)
                energy->item(i, c)->setFont([&] {
                    auto f = energy->item(i, c)->font();
                    f.setBold(true);
                    return f;
                }());
    }
    energyNote->setText(
        energies.isEmpty()
            ? QString()
            : tr("ΔEは %1 行目（太字）を基準にした差です。手法の異なる行どうしの差には意味がありま"
                 "せん。エネルギーは出力順で、最適化ステップとの1対1対応ではありません。"
                 "1 Eh = 627.509 kcal/mol = 2625.50 kJ/mol = 27.2114 eV。")
                  .arg(reference + 1));
}
} // namespace mf
