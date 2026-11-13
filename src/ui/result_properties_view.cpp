// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/result_properties_view.h"
#include "foundation/i18n.h"
#include "model/units.h"
#include "results/result_path.h"
#include "results/result_properties.h"
#include "ui/result_detail.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace mf {
using namespace result_detail;
namespace {
QString solvationText(const QJsonObject &s) {
    QStringList lines;
    if (s.contains("smd_dg"))
        lines << tr("SMD溶媒和自由エネルギー（1 M基準）: %1 kcal/mol、溶液中の全自由エネルギー %2 Eh")
                     .arg(fixed(s["smd_dg"].toDouble() * HartreeToKcal, 2), real(s["smd_total"].toDouble()));
    else if (s.contains("gas") && s.contains("solution"))
        lines << tr("COSMO溶媒和エネルギー（静電項、溶液 − 気相）: %1 kcal/mol")
                     .arg(fixed((s["solution"].toDouble() - s["gas"].toDouble()) * HartreeToKcal, 2));
    return lines.join('\n');
}
} // namespace
QString resultPropertyLines(const ResultData &data) {
    QString text;
    if (data.properties.contains("bsse")) {
        const auto b = data.properties.value("bsse").toObject();
        text += tr("BSSE（counterpoise）補正: %1 kcal/mol、補正後 %2 Eh（補正前 %3 Eh）\n")
                    .arg(fixed(b["error"].toDouble() * HartreeToKcal, 3), real(b["corrected"].toDouble()),
                         real(b["supermolecular"].toDouble()));
        if (b.contains("interaction"))
            text += tr("相互作用エネルギー（単量体の和との差）: 補正後 %1 kcal/mol（補正前 %2 kcal/mol）\n")
                        .arg(fixed(b["interaction"].toDouble() * HartreeToKcal, 3),
                             fixed(b["interaction_uncorrected"].toDouble() * HartreeToKcal, 3));
    }
    if (const auto s = solvationText(data.properties.value("solvation").toObject()); !s.isEmpty())
        text += s + '\n';
    if (data.properties.contains("polarizability")) {
        const auto p = data.properties.value("polarizability").toObject();
        constexpr double BohrCubed = 0.1481847; // Å³ per a.u. of polarizability volume.
        text += tr("静的分極率: 等方 %1 a.u.（%2 Å³）、異方性 %3 a.u.\n")
                    .arg(fixed(p["isotropic"].toDouble(), 4), fixed(p["isotropic"].toDouble() * BohrCubed, 4),
                         fixed(p["anisotropy"].toDouble(), 4));
    }
    if (data.properties.contains("scan"))
        text += tr("座標スキャン: %n点（「経路・スキャン」に表示）\n", nullptr,
                   int(data.properties.value("scan").toArray().size()));
    if (data.properties.contains("neb")) {
        const auto neb = data.properties.value("neb").toObject();
        const auto path = resultPath(data, ResultPathKind::Neb);
        text +=
            tr("NEB: 像 %n個、ステップ %1%2（「経路・スキャン」に表示）\n", nullptr, int(path.points.size()))
                .arg(neb["iteration"].toInt())
                .arg(!neb.contains("converged")  ? QString()
                     : neb["converged"].toBool() ? tr("、収束")
                                                 : tr("、未収束"));
        // The ends are fixed structures: the highest of the images between them.
        if (const int highest = path.highest(PathEnds::Excluded); highest >= 0)
            text += tr("NEBの最高点: 像 %1（始点から %2 kcal/mol）\n")
                        .arg(highest + 1)
                        .arg(fixed(path.relative(highest) * HartreeToKcal, 2));
    }
    if (data.properties.contains("irc")) {
        const auto irc = data.properties.value("irc").toObject();
        const auto directions = irc["directions"].toArray();
        const bool both = directions.size() != 1;
        const bool forward = !both && directions[0] == "forward";
        text += tr("IRC: %n点%1（「経路・スキャン」に表示）\n", nullptr, int(irc["points"].toArray().size()))
                    .arg(irc["complete"].toBool() ? (both      ? tr("、両方向とも収束")
                                                     : forward ? tr("、前進方向で収束")
                                                               : tr("、後退方向で収束"))
                                                  : (both ? tr("、収束メッセージは両方向にありません")
                                                          : tr("、収束メッセージがありません")));
    }
    return text;
}
ResultPropertiesView::ResultPropertiesView(QWidget *parent) : QWidget(parent) {
    auto *ppl = new QVBoxLayout(this);
    propertySummary = new QLabel;
    propertySummary->setObjectName("resultsPropertySummary");
    propertySummary->setTextFormat(Qt::PlainText);
    propertySummary->setWordWrap(true);
    propertySummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ppl->addWidget(propertySummary);
    atomProperties = table({tr("番号"), tr("元素"), tr("Mulliken電荷"), tr("原子価"), tr("ESP電荷"),
                            tr("RESP電荷"), tr("NMR遮蔽 / ppm"), tr("異方性 / ppm")},
                           "resultsAtomProperties");
    atomProperties->verticalHeader()->hide();
    ppl->addWidget(atomProperties, 1);
    // The bond indices of the Mulliken analysis: only for a log with them.
    bondBlock = new QWidget;
    auto *bl = new QVBoxLayout(bondBlock);
    bl->setContentsMargins(0, 0, 0, 0);
    auto *bondHead = new QHBoxLayout;
    bondTitle = new QLabel;
    bondTitle->setObjectName("resultsBondIndicesTitle");
    bondHead->addWidget(bondTitle, 1);
    allBondIndices = new QCheckBox(tr("すべての原子対を表示"));
    allBondIndices->setObjectName("resultsBondIndicesAll");
    allBondIndices->setToolTip(tr("オフのときは、結合指数の絶対値が0.05以上の原子対だけを表示します。"));
    bondHead->addWidget(allBondIndices);
    bl->addLayout(bondHead);
    bondIndices = table({tr("原子対"), tr("結合指数"), tr("距離 / Å")}, "resultsBondIndices");
    bondIndices->verticalHeader()->hide();
    bl->addWidget(bondIndices, 1);
    auto *bondHint =
        new QLabel(tr("NWChemのMulliken解析が出力する結合指数（Mayerの結合次数）と原子価です。基底関数に"
                      "依存し、diffuse関数を含む基底では信頼できません。距離は最後の構造の値で、行を選ぶと"
                      "その2原子を3Dビューで選択します。"));
    bondHint->setWordWrap(true);
    bl->addWidget(bondHint);
    ppl->addWidget(bondBlock, 1);
    connect(allBondIndices, &QCheckBox::toggled, this, &ResultPropertiesView::showBondIndices);
    connect(bondIndices, &QTableWidget::currentCellChanged, this, [this](int row) {
        const auto *item = row >= 0 ? bondIndices->item(row, 0) : nullptr;
        if (!item || data.frames.isEmpty())
            return;
        // The last structure, which the analysis is of, with the two atoms picked as for a measurement.
        const auto pair = item->data(Qt::UserRole).toPoint();
        if (pair.x() < 0 || pair.y() >= data.frames.last().molecule.atoms.size())
            return;
        emit atomPairSelected(pair.x(), pair.y());
    });
    // NMR spin–spin couplings: only for a log with them.
    couplingBlock = new QWidget;
    auto *jl = new QVBoxLayout(couplingBlock);
    jl->setContentsMargins(0, 0, 0, 0);
    couplingTitle = new QLabel;
    couplingTitle->setObjectName("resultsCouplingsTitle");
    jl->addWidget(couplingTitle);
    couplings = table({tr("原子の組"), tr("核種"), "J / Hz", tr("距離 / Å")}, "resultsCouplings");
    couplings->verticalHeader()->hide();
    couplings->setSortingEnabled(false);
    jl->addWidget(couplings, 1);
    auto *couplingHint =
        new QLabel(tr("NWChemが計算した間接スピン–スピン結合定数（FC・SD・PSO・DSO項の和の等方値）"
                      "です。核種はNWChemが使った同位体です。行を選ぶとその2原子を3Dビューで"
                      "選択します。"));
    couplingHint->setWordWrap(true);
    jl->addWidget(couplingHint);
    ppl->addWidget(couplingBlock, 1);
    connect(couplings, &QTableWidget::currentCellChanged, this, [this](int row) {
        const auto *item = row >= 0 ? couplings->item(row, 0) : nullptr;
        if (!item || data.frames.isEmpty())
            return;
        const auto pair = item->data(Qt::UserRole).toPoint();
        if (pair.x() < 0 || pair.y() >= data.frames.last().molecule.atoms.size())
            return;
        emit atomPairSelected(pair.x(), pair.y());
    });
}
void ResultPropertiesView::showBondIndices() {
    const auto list = data.properties.value("bond_indices").toArray();
    const Document *molecule = data.frames.isEmpty() ? nullptr : &data.frames.last().molecule;
    struct Pair {
        int a, b; // From 0.
        double value;
    };
    QVector<Pair> pairs;
    int notable = 0;
    for (const auto &value : list) {
        const auto o = value.toObject();
        const Pair pair{o["a"].toInt() - 1, o["b"].toInt() - 1, o["value"].toDouble()};
        if (pair.a < 0 || pair.b <= pair.a)
            continue;
        const bool bond = std::abs(pair.value) >= NotableBondIndex;
        notable += bond;
        if (bond || allBondIndices->isChecked())
            pairs << pair;
    }
    // The largest first; equal ones in the order of the atoms.
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const Pair &p, const Pair &q) { return p.value > q.value; });
    const auto label = [&](int atom) {
        return (molecule && atom < molecule->atoms.size() ? molecule->atoms[atom].element : QString()) +
               QString::number(atom + 1);
    };
    const QSignalBlocker guard(bondIndices);
    bondIndices->setRowCount(pairs.size());
    for (int i = 0; i < pairs.size(); ++i) {
        const auto &p = pairs[i];
        const bool known = molecule && p.b < molecule->atoms.size();
        row(bondIndices, i,
            {label(p.a) + "–" + label(p.b), fixed(p.value, 5),
             known ? fixed((molecule->atoms[p.a].position - molecule->atoms[p.b].position).norm(), 4)
                   : QString("—")});
        bondIndices->item(i, 0)->setData(Qt::UserRole, known ? QPoint(p.a, p.b) : QPoint(-1, -1));
    }
    bondIndices->clearSelection();
    bondIndices->setCurrentCell(-1, -1);
    bondBlock->setVisible(!list.isEmpty());
    const int omitted = data.properties.value("bond_indices_omitted").toInt();
    bondTitle->setText(
        omitted > 0 ? tr("結合指数（絶対値0.05以上: %1対。原子対が多いため、それ未満の %2対は省いています）")
                          .arg(notable)
                          .arg(omitted)
                    : tr("結合指数（絶対値0.05以上: %1対、全 %2対）").arg(notable).arg(list.size()));
    allBondIndices->setVisible(omitted == 0 && notable < list.size());
}
void ResultPropertiesView::setResult(const ResultData &result) {
    data = result;
    const auto mulliken = data.properties.value("mulliken").toArray(),
               esp = data.properties.value("esp").toArray(), resp = data.properties.value("resp").toArray(),
               shielding = data.properties.value("shielding").toArray();
    QMap<int, QJsonObject> shieldingByAtom;
    for (const auto &value : shielding) {
        const auto s = value.toObject();
        if (s["atom"].toInt() > 0 && s["atom"].toInt() <= 100000)
            shieldingByAtom[s["atom"].toInt() - 1] = s;
    }
    const auto valency = data.properties.value("valency").toArray();
    const int count = std::max({int(mulliken.size()), int(valency.size()), int(esp.size()), int(resp.size()),
                                shieldingByAtom.isEmpty() ? 0 : shieldingByAtom.lastKey() + 1});
    const Document *molecule = data.frames.isEmpty() ? nullptr : &data.frames.last().molecule;
    atomProperties->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const auto nmr = shieldingByAtom.value(i);
        const QString symbol =
            molecule && i < molecule->atoms.size() ? molecule->atoms[i].element : nmr["element"].toString();
        const auto charge = [&](const QJsonArray &list) {
            return i < list.size() ? fixed(list[i].toDouble(), 4) : QString("—");
        };
        row(atomProperties, i,
            {QString::number(i + 1), symbol, charge(mulliken), charge(valency), charge(esp), charge(resp),
             nmr.contains("isotropic") ? fixed(nmr["isotropic"].toDouble(), 3) : "—",
             nmr.contains("anisotropy") ? fixed(nmr["anisotropy"].toDouble(), 3) : "—"});
    }
    atomProperties->setColumnHidden(3, valency.isEmpty()); // Only of a Mulliken analysis.
    showBondIndices();
    {
        const auto list = data.properties.value("spinspin").toArray();
        const QSignalBlocker guard(couplings);
        couplings->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const auto c = list[i].toObject();
            const int a = c["a"].toInt() - 1, b = c["b"].toInt() - 1;
            const bool known =
                molecule && a >= 0 && b >= 0 && a < molecule->atoms.size() && b < molecule->atoms.size();
            const auto label = [&](int atom) {
                return (known ? molecule->atoms[atom].element : QString()) + QString::number(atom + 1);
            };
            row(couplings, i,
                {label(a) + "–" + label(b), c["isotope_a"].toString() + " / " + c["isotope_b"].toString(),
                 fixed(c["hz"].toDouble(), 2),
                 known ? fixed((molecule->atoms[a].position - molecule->atoms[b].position).norm(), 4)
                       : QString("—")});
            couplings->item(i, 0)->setData(Qt::UserRole, known ? QPoint(a, b) : QPoint(-1, -1));
        }
        couplingBlock->setVisible(!list.isEmpty());
        couplingTitle->setText(tr("J結合（スピン–スピン結合定数、%n 組）", nullptr, int(list.size())));
    }
    auto text = resultPropertyLines(data);
    if (const auto d = resultDipole(data))
        text += tr("双極子モーメント: %1 D（x %2 / y %3 / z %4 a.u.）\n")
                    .arg(fixed(d->debye, 4), real(d->au.x()), real(d->au.y()), real(d->au.z()));
    if (data.properties.contains("polarizability")) {
        const auto t = data.properties.value("polarizability").toObject()["tensor"].toArray();
        QStringList rows;
        for (int r = 0; r < 3; ++r)
            rows << QString("%1 %2 %3")
                        .arg(fixed(t[3 * r].toDouble(), 4), fixed(t[3 * r + 1].toDouble(), 4),
                             fixed(t[3 * r + 2].toDouble(), 4));
        text += tr("分極率テンソル / a.u.: [%1]\n").arg(rows.join(" / "));
    }
    if (!shielding.isEmpty())
        text +=
            tr("NMRは絶対遮蔽定数です。化学シフトは同じ条件で計算した基準物質（TMS等）の値との差です。\n");
    if (text.isEmpty())
        text = tr(
            "このログには物性の結果がありません。「2 計算条件」の「物性」・溶媒・BSSE補正で計算できます。");
    propertySummary->setText(text.trimmed());
}
} // namespace mf
