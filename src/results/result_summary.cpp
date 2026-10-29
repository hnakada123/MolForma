// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_summary.h"
#include "model/i18n.h"

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
