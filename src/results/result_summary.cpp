// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_summary.h"
#include "foundation/i18n.h"
#include <algorithm>

namespace mf {
QVector<ResultModeKind> resultModeKinds(const ResultData &data) {
    const auto rigid = resultRigidModes(data);
    QVector<ResultModeKind> kinds;
    for (int i = 0; i < data.modes.size(); ++i) {
        const double frequency = data.modes[i].frequency;
        kinds << (rigid.value(i)                              ? ResultModeKind::Rigid
                  : frequency < SignificantImaginaryFrequency ? ResultModeKind::SignificantImaginary
                  : frequency < 0                             ? ResultModeKind::Imaginary
                                                              : ResultModeKind::Vibration);
    }
    return kinds;
}
ResultModeCounts resultModeCounts(const QVector<ResultModeKind> &kinds) {
    ResultModeCounts counts;
    for (const auto kind : kinds) {
        if (kind == ResultModeKind::Rigid) {
            ++counts.rigid;
            continue;
        }
        ++counts.vibrations;
        counts.imaginary += imaginaryMode(kind);
        counts.significant += kind == ResultModeKind::SignificantImaginary;
    }
    return counts;
}
QString resultImaginaryText(const ResultModeCounts &counts) {
    if (!counts.imaginary)
        return tr("虚振動なし");
    // The threshold is SignificantImaginaryFrequency.
    return tr("%1（−10 cm⁻¹未満 %n個）", nullptr, counts.significant)
        .arg(tr("虚振動 %n個", nullptr, counts.imaginary));
}
std::array<ResultCriterion, 4> resultCriteria(const ResultStep &step, const QJsonObject &thresholds) {
    std::array<ResultCriterion, 4> criteria{{{"gmax", "Gmax", step.gmax},
                                             {"grms", "Grms", step.grms},
                                             {"xrms", "Xrms", step.xrms},
                                             {"xmax", "Xmax", step.xmax}}};
    for (auto &c : criteria) {
        if (thresholds.contains(c.key))
            c.threshold = thresholds[c.key].toDouble();
        c.measured = c.key[0] == 'g' || step.step > 0;
    }
    return criteria;
}
QVector<ResultTransition> resultTransitions(const ResultExcitation &state) {
    QVector<ResultTransition> pairs;
    for (const auto &c : state.contributions) {
        auto pair = std::find_if(pairs.begin(), pairs.end(), [&](const ResultTransition &p) {
            return p.from == c.from && p.to == c.to && p.spin == c.spin;
        });
        if (pair == pairs.end())
            pair = pairs.insert(pairs.end(), ResultTransition{c.from, c.to, c.spin});
        (c.deexcitation ? pair->y : pair->x) = c.coefficient;
    }
    const auto share = [](const ResultTransition &p) {
        return p.x * p.x - (std::isfinite(p.y) ? p.y * p.y : 0);
    };
    double sum = 0;
    for (const auto &p : std::as_const(pairs))
        sum += share(p);
    if (sum > 0)
        for (auto &p : pairs)
            p.weight = 100 * share(p) / sum;
    std::stable_sort(pairs.begin(), pairs.end(), [&](const ResultTransition &p, const ResultTransition &q) {
        return share(p) > share(q);
    });
    return pairs;
}
int resultHomoNumber(const ResultData &data, const QString &spin) {
    const bool beta = spin == "beta";
    const auto electrons = data.properties.value("tddft").toObject();
    if (const auto count = electrons[beta ? "beta_electrons" : "alpha_electrons"]; count.isDouble())
        return count.toInt();
    // The analysis lists the orbitals around the frontier: the last occupied one before an empty one.
    int homo = 0, lumo = 0;
    for (const auto &o : data.orbitals) {
        if (o.spin != (beta ? "Beta" : "Alpha") || o.number <= 0)
            continue;
        if (o.occupation > 1e-8)
            homo = std::max(homo, o.number);
        else if (!lumo || o.number < lumo)
            lumo = o.number;
    }
    return lumo == homo + 1 ? homo : 0;
}
QString resultFrontierLabel(int number, int homo) {
    if (homo <= 0 || number <= 0)
        return QString::number(number);
    if (number <= homo)
        return number == homo ? QString("HOMO") : QString("HOMO−%1").arg(homo - number);
    return number == homo + 1 ? QString("LUMO") : QString("LUMO+%1").arg(number - homo - 1);
}
QString resultTransitionText(const ResultData &data, const ResultTransition &t) {
    const int homo = resultHomoNumber(data, t.spin);
    return (t.spin == "alpha"  ? "α "
            : t.spin == "beta" ? "β "
                               : "") +
           QString("%1 → %2").arg(resultFrontierLabel(t.from, homo), resultFrontierLabel(t.to, homo));
}
QString resultBriefSummary(const ResultData &data) {
    QStringList parts;
    if (data.properties.contains("optimization"))
        parts << (data.properties.value("optimization") == "converged" ? tr("最適化: 収束")
                                                                       : tr("最適化: 収束メッセージなし"));
    if (const auto final = data.properties["final_energy"].toObject(); !final.isEmpty())
        parts << tr("E = %1 Eh（%2）")
                     .arg(QString::number(final["hartree"].toDouble(), 'g', 12).replace('-', "−"),
                          final["method"].toString());
    if (!data.modes.isEmpty())
        parts << resultImaginaryText(resultModeCounts(resultModeKinds(data)));
    if (!data.excitations.isEmpty())
        parts << tr("励起状態 %n件", nullptr, int(data.excitations.size()));
    return parts.join(" · ");
}
QString resultMessage(const ResultData &data) {
    if (data.source.isEmpty())
        return {};
    const auto brief = resultBriefSummary(data);
    return brief.isEmpty() ? data.status : data.status + "\n" + brief;
}
} // namespace mf
