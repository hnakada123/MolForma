// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/charge_page.h"
#include "editing/operations.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "nwchem/nwchem.h"
#include "ui/chrome_layouts.h"
#include "ui/settings_widgets.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
static Vec fields(QDoubleSpinBox *const *spins) {
    return {spins[0]->value(), spins[1]->value(), spins[2]->value()};
}
ChargePage::ChargePage(EditorContext &editorServices, UiServices &uiServices,
                       WorkspaceActions &workspaceActions, QWidget *parent)
    : PropertyPage(editorServices, uiServices, parent), workspace(workspaceActions) {
    auto *f = form();
    // The total charge and multiplicity are settings of "2 計算条件": here they are shown with the
    // formal charges of the atoms and the electrons they give, with the way there.
    section(f, QT_TR_NOOP("分子の電荷"));
    chargeState = new QLabel;
    chargeState->setObjectName("moleculeChargeState");
    auto *chargeLink = linkButton(tr("2 計算条件で変更"), "moleculeChargeLink");
    connect(chargeLink, &QPushButton::clicked, this,
            [this] { workspace.showSetting(SettingTarget::Calculation, "charge"); });
    // The value and the link wrap: the link goes under the value where the column is narrow.
    auto *chargeRow = new QWidget;
    auto *chargeRowLayout = new FlowLayout(chargeRow, 10, 4);
    chargeRowLayout->setContentsMargins(0, 0, 0, 0);
    chargeRowLayout->addWidget(chargeState);
    chargeRowLayout->addWidget(chargeLink);
    f->addRow(tr("全電荷・多重度"), chargeRow);
    chargeSummary = new QLabel;
    chargeSummary->setObjectName("moleculeChargeSummary");
    chargeSummary->setTextFormat(Qt::PlainText);
    chargeSummary->setWordWrap(true);
    f->addRow(chargeSummary);
    inputState = new QLabel;
    inputState->setObjectName("moleculeInputState");
    inputState->setWordWrap(true);
    f->addRow(inputState);
    // With coordination centers, their oxidation numbers take the place of the centers' formal charges.
    button(
        f, tr("形式電荷の合計を全電荷に適用"),
        [this] {
            editorContext.edit(tr("全電荷を合わせる"), [](Document &d) { d.charge = d.expectedCharge(); });
        },
        "applyMoleculeFormalCharge")
        ->setToolTip(
            tr("原子の形式電荷の合計（配位中心は酸化数）を、計算の全電荷（「2 計算条件」）にします。"));
    auto *preview = linkButton(tr("4 入力プレビューで確認"), "moleculeInputPreview");
    connect(preview, &QPushButton::clicked, this, [this] { workspace.showInputLine(); });
    f->addRow(preview);
    section(f, QT_TR_NOOP("外部点電荷"));
    pointChargeState = new QLabel;
    pointChargeState->setObjectName("moleculePointChargeState");
    auto *pointLink = linkButton(tr("2 計算条件で設定"), "moleculePointChargeLink");
    pointLink->setToolTip(tr("点電荷をNWChem入力に含めるかは「2 計算条件」の「周囲の環境」で設定します。"));
    connect(pointLink, &QPushButton::clicked, this,
            [this] { workspace.showSetting(SettingTarget::Calculation, "include_point_charges"); });
    auto *pointRow = new QWidget;
    auto *pointRowLayout = new FlowLayout(pointRow, 10, 4);
    pointRowLayout->setContentsMargins(0, 0, 0, 0);
    pointRowLayout->addWidget(pointChargeState);
    pointRowLayout->addWidget(pointLink);
    f->addRow(pointRow);
    pointList = new QListWidget;
    pointList->setObjectName("pointChargeList");
    pointList->setMaximumHeight(160);
    f->addRow(pointList);
    pointEmpty =
        new QLabel(tr("点電荷はまだありません。「点電荷」ツールで3D画面の空白をクリックするか、下の欄に"
                      "位置と電荷を入れて「点電荷を追加」を押します。"));
    pointEmpty->setObjectName("pointChargeEmpty");
    pointEmpty->setProperty("muted", true);
    pointEmpty->setWordWrap(true);
    f->addRow(pointEmpty);
    // The position and charge two by two, under their names.
    auto *fieldsBox = new QWidget;
    auto *fieldGrid = new QGridLayout(fieldsBox);
    fieldGrid->setContentsMargins(0, 0, 0, 0);
    fieldGrid->setHorizontalSpacing(8);
    fieldGrid->setVerticalSpacing(3);
    const auto field = [](const QString &name) {
        auto *n = new QDoubleSpinBox;
        n->setObjectName(name);
        n->setDecimals(5);
        n->setRange(-10000, 10000);
        n->setKeyboardTracking(false);
        return n;
    };
    for (int i = 0; i < 4; ++i) {
        auto *spin = i < 3 ? (chargeFields[i] = field(QString("pointCharge") + QString("XYZ")[i]))
                           : (chargeValue = field("pointChargeValue"));
        auto *label = new QLabel(i < 3 ? QString("XYZ")[i] + QString(" / Å") : tr("電荷 / e"));
        label->setProperty("caption", true);
        label->setBuddy(spin);
        fieldGrid->addWidget(label, (i / 2) * 2, i % 2);
        fieldGrid->addWidget(spin, (i / 2) * 2 + 1, i % 2);
    }
    chargeValue->setValue(.5);
    f->addRow(fieldsBox);
    connect(pointList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        for (const auto &q : editorContext.displayDocument().pointCharges)
            if (q.id == item->data(Qt::UserRole).toString()) {
                for (int i = 0; i < 3; ++i)
                    chargeFields[i]->setValue(q.position[i]);
                chargeValue->setValue(q.charge);
            }
    });
    button(
        f, tr("点電荷を追加"),
        [this] {
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            editorContext.edit(tr("点電荷を追加"), [=](Document &d) { addPointCharge(d, p, q); });
        },
        "addPointCharge")
        ->setProperty("primary", true);
    button(
        f, tr("点電荷を更新"),
        [this] {
            if (!pointList->currentItem())
                fail(tr("点電荷を選択してください。"));
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            editorContext.edit(tr("点電荷を更新"), [=](Document &d) { setPointCharge(d, id, p, q); });
        },
        "updatePointCharge");
    button(
        f, tr("点電荷を削除"),
        [this] {
            if (!pointList->currentItem())
                fail(tr("点電荷を選択してください。"));
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            editorContext.edit(tr("点電荷を削除"), [=](Document &d) { removePointCharge(d, id); });
        },
        "deletePointCharge");
    enableWithSelection(button(f, tr("選択した水をTIP3P点電荷へ"),
                               [this] {
                                   auto ids = requireSelection();
                                   editorContext.edit(
                                       tr("水を点電荷へ変換"), [=](Document &d) { waterToCharges(d, ids); },
                                       true);
                               }),
                        1);
    button(f, tr("点電荷をファイルに書き出す…"), [this] {
        if (!editorContext.resolveCandidate())
            return;
        QString p = QFileDialog::getSaveFileName(this, tr("点電荷を書き出す"), "charges.dat",
                                                 tr("点電荷 (*.dat *.txt)"));
        if (p.isEmpty())
            return;
        QString text = "# MolForma point charges; units=angstrom; x y z q\n";
        for (const auto &q : editorContext.document().pointCharges)
            text += QString("%1 %2 %3 %4\n")
                        .arg(q.position.x(), 0, 'f', 10)
                        .arg(q.position.y(), 0, 'f', 10)
                        .arg(q.position.z(), 0, 'f', 10)
                        .arg(q.charge, 0, 'g', 15);
        writeFile(p, text.toUtf8());
    });
}
void ChargePage::refresh() {
    const auto &d = editorContext.displayDocument();
    QString current = pointList->currentItem() ? pointList->currentItem()->data(Qt::UserRole).toString() : "";
    pointList->clear();
    int index = 0;
    for (const auto &q : d.pointCharges) {
        auto *item = new QListWidgetItem(QString("%1: %2 e  (%3, %4, %5)")
                                             .arg(++index)
                                             .arg(q.charge)
                                             .arg(q.position.x(), 0, 'f', 2)
                                             .arg(q.position.y(), 0, 'f', 2)
                                             .arg(q.position.z(), 0, 'f', 2),
                                         pointList);
        item->setData(Qt::UserRole, q.id);
        if (q.id == current)
            pointList->setCurrentItem(item);
    }
    pointList->setVisible(index > 0);
    pointEmpty->setVisible(index == 0);
    chargeState->setText(tr("%1 · 多重度 %2").arg(d.charge).arg(d.multiplicity));
    pointChargeState->setText(nwchemSettings(d)["include_point_charges"].toBool()
                                  ? tr("NWChem入力に含める")
                                  : tr("NWChem入力に含めない"));
    widget()->setEnabled(!editorContext.hasCandidate() && !editorContext.chemistryRunning());
    if (auto *apply = findChild<QPushButton *>("applyMoleculeFormalCharge"))
        apply->setText(d.hasCenters()
                           ? tr("酸化数と形式電荷の合計（%1）を全電荷に適用").arg(d.expectedCharge())
                           : tr("形式電荷の合計を全電荷に適用"));
    const auto check = validateNwchem(d);
    QStringList text{(check.ecpElectrons ? tr("形式電荷の合計 %1 · 電子数 %2（ECP適用後）")
                                         : tr("形式電荷の合計 %1 · 電子数 %2"))
                         .arg(d.formalCharge())
                         .arg(check.electrons)};
    if (check.errors.isEmpty())
        text << QString("α %1 / β %2").arg(check.alpha).arg(check.beta);
    for (const auto &error : check.errors)
        text << tr("エラー: %1").arg(error);
    for (const auto &warning : check.warnings)
        text << tr("確認: %1").arg(warning);
    if (editorContext.hasCandidate())
        text.prepend(tr("未確定候補です。確定・取消後に設定を変更できます。"));
    else if (editorContext.chemistryRunning())
        text.prepend(tr("分子の整形中です。終了後に設定を変更できます。"));
    chargeSummary->setText(text.join('\n'));
    // The outcome of the checks as the cards of the settings tabs say it.
    const bool ok = check.errors.isEmpty();
    inputState->setText(ok ? tr("✓ 入力を生成できます")
                           : tr("入力を生成できません（エラー %n 件）", nullptr, int(check.errors.size())));
    if (inputState->property("good").toBool() != ok) {
        inputState->setProperty("good", ok);
        inputState->setProperty("bad", !ok);
        repolish(inputState);
    }
}
} // namespace mf
