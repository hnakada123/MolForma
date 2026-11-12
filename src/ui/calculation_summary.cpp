// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/calculation_summary.h"
#include "foundation/i18n.h"
#include "nwchem/nwchem.h"
#include "ui/chrome_layouts.h"
#include "ui/theme.h"
#include <QVBoxLayout>
#include <tuple>

namespace mf {
QString calculationBrief(const Document &d) {
    const auto s = nwchemSettings(d);
    QString task = s["task"].toString(), solvent;
    for (const auto &c : nwchemChoices("task"))
        if (c.value == task)
            task = c.label;
    if (s["solvent"] != "none") {
        // The names without their notes in parentheses (" (" in English).
        const auto opening = tr("（");
        for (const auto &c : nwchemChoices("solvent"))
            if (c.value == s["solvent"].toString())
                solvent = " · " + c.label.section(opening, 0, 0);
        for (const auto &c : nwchemChoices("solvent_name"))
            if (c.value == s["solvent_name"].toString() && s["solvent_name"] != "dielectric")
                solvent = tr("%1（%2）").arg(solvent, c.label.section(opening, 0, 0));
    }
    return (s["method"] == "dft" ? nwchemFunctionalLabel(s) : s["method"].toString().toUpper()) + " / " +
           nwchemBasisLabel(s) + " · " + task + solvent;
}

CalculationSummary::CalculationSummary(const QString &prefix, const QString &caption,
                                       const std::function<void(const QString &)> &openLink, QWidget *parent)
    : SettingsCard({}, parent) {
    setObjectName(prefix + "SummaryCard");
    body()->setSpacing(4);
    auto *kind = new QLabel(caption);
    kind->setProperty("caption", true);
    body()->addWidget(kind);
    title = new QLabel;
    title->setObjectName(prefix + "Summary");
    title->setProperty("cardTitle", true);
    title->setWordWrap(true);
    body()->addWidget(title);
    task = new QLabel;
    task->setObjectName(prefix + "SummaryTask");
    task->setWordWrap(true);
    body()->addWidget(task);
    auto *facts = new FormLayout;
    facts->setContentsMargins(0, 6, 0, 0);
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label, key] : {std::tuple{QT_TR_NOOP("組成"), &formula, "Formula"},
                                           {QT_TR_NOOP("電子数"), &electrons, "Electrons"},
                                           {QT_TR_NOOP("電荷・多重度"), &charge, "Charge"},
                                           {QT_TR_NOOP("メモリ"), &memory, "Memory"}}) {
        *label = new QLabel;
        (*label)->setObjectName(prefix + "Summary" + key);
        (*label)->setWordWrap(true);
        auto *term = new QLabel(tr(name));
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    memory->setTextFormat(Qt::RichText);
    connect(memory, &QLabel::linkActivated, this, [openLink](const QString &link) { openLink(link); });
    body()->addLayout(facts);
}
void CalculationSummary::update(const Document &d) {
    const auto s = nwchemSettings(d);
    const auto check = validateNwchem(d);
    const auto brief = calculationBrief(d);
    title->setText(brief.section(" · ", 0, 0));
    task->setText(brief.section(" · ", 1));
    const QString method = s["method"].toString();
    const QString reference = method == "dft" && check.reference != "rohf"
                                  ? (check.reference == "rhf" ? "RKS" : "UKS")
                                  : check.reference.toUpper();
    formula->setText(d.atoms.isEmpty() ? tr("原子なし")
                                       : tr("%1 · %n 原子", nullptr, int(d.atoms.size())).arg(d.formula()));
    electrons->setText((check.ecpElectrons ? tr("%1（α %2 / β %3）ECP適用後") : tr("%1（α %2 / β %3）"))
                           .arg(check.electrons)
                           .arg(check.alpha)
                           .arg(check.beta));
    const auto charges = QString::number(d.charge), multiplicity = QString::number(d.multiplicity);
    charge->setText(reference.isEmpty() ? QString("%1 · %2").arg(charges, multiplicity)
                                        : tr("%1 · %2（%3）").arg(charges, multiplicity, reference));
    // The link on a line of its own, never split.
    memory->setText(tr("%1 MB / MPIプロセス<br><a href=\"memory\" style=\"color:%2; "
                       "text-decoration:none\">3 実行設定で変更</a>")
                        .arg(s["memory_mb"].toInt())
                        .arg(ChromeColors(palette()).accentText.name()));
}
} // namespace mf
