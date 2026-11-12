// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/xtb_dialog.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "ui/editor_context.h"
#include <QtWidgets>

namespace mf {
std::optional<XtbOptions> chooseXtbOptions(QWidget *parent, const EditorContext &context,
                                           const UiServices &services) {
    if (context.hasCandidate() || context.chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行してください。"));
    QDialog dialog(parent);
    dialog.setObjectName("xtbDialog");
    dialog.setWindowTitle(tr("xTBで構造を最適化"));
    auto *layout = new QVBoxLayout(&dialog);
    const auto program = xtbExecutable(services.preference(Preference::xtbExecutable).toString());
    auto *intro = new QLabel(
        tr("半経験的なGFN-xTB（xtbプログラム）で構造を最適化し、候補として表示します。金属錯体・配位結合を"
           "含む構造も扱えます（Rnまで）。DFT計算の前の粗い整形に使います。電荷・多重度は「2 計算条件」の"
           "値、固定原子は動かしません。外部点電荷は含めません。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *found =
        new QLabel(program.isEmpty() ? tr("xtbが見つかりません。「編集 → 環境設定」の「xTB実行ファイル」"
                                          "で指定してください。")
                                     : tr("実行ファイル: %1").arg(program));
    found->setObjectName("xtbExecutable");
    found->setWordWrap(true);
    found->setTextFormat(Qt::PlainText);
    found->setProperty("muted", true);
    layout->addWidget(found);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *method = new QComboBox;
    method->setObjectName("xtbMethod");
    for (const auto &key : xtbMethods())
        method->addItem(xtbMethodName(key), key);
    method->setToolTip(tr("GFN2-xTBが標準です。GFN-FFは力場で、大きな構造（数百原子以上）を速く整えます。"));
    form->addRow(tr("手法"), method);
    auto *level = new QComboBox;
    level->setObjectName("xtbLevel");
    for (const auto &key : xtbLevels())
        level->addItem(xtbLevelName(key), key);
    level->setCurrentIndex(level->findData("normal"));
    form->addRow(tr("収束の基準"), level);
    auto *solvent = new QComboBox;
    solvent->setObjectName("xtbSolvent");
    solvent->addItem(tr("なし（気相）"), QString());
    for (const auto &name : xtbSolvents())
        solvent->addItem(name, name);
    solvent->setToolTip(tr("ALPB暗黙溶媒モデルの溶媒名（xtbの名前）です。"));
    form->addRow(tr("溶媒（ALPB）"), solvent);
    auto *selected = new QCheckBox(tr("選択した原子だけを動かす（ほかは固定）"));
    selected->setObjectName("xtbSelectionOnly");
    selected->setEnabled(!context.selection().isEmpty());
    form->addRow(QString(), selected);
    auto *state = new QLabel(
        tr("電荷 %1・多重度 %2（「2 計算条件」）・%n 原子", nullptr, int(context.document().atoms.size()))
            .arg(context.document().charge)
            .arg(context.document().multiplicity));
    state->setProperty("muted", true);
    form->addRow(QString(), state);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("最適化"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!program.isEmpty());
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    XtbOptions options;
    options.method = method->currentData().toString();
    options.level = level->currentData().toString();
    options.solvent = solvent->currentData().toString();
    if (selected->isChecked())
        options.moving = context.selection();
    return options;
}
} // namespace mf
