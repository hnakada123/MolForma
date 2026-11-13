// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/molecule_dialogs.h"
#include "editing/coordinates.h"
#include "editing/library.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/solvation.h"
#include "editing/stereochemistry.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "ui/chrome_layouts.h"
#include "ui/editor_context.h"
#include <QtWidgets>

namespace mf {
QString askSmiles(QWidget *parent, const QString &title, const QString &label, const QString &initial) {
    QInputDialog dialog(parent);
    dialog.setObjectName("smilesDialog");
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(initial);
    dialog.setOkButtonText(tr("3Dで作成"));
    dialog.resize(560, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted ? dialog.textValue().trimmed() : QString();
}

void showStructureReport(QWidget *parent, const Document &document) {
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("構造の確認"));
    dialog.resize(680, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(structureReport(document));
    text->setReadOnly(true);
    layout->addWidget(text);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

bool askSuperposition(QWidget *parent, const QString &name, const Superposition &fit) {
    QMessageBox box(QMessageBox::Information, tr("構造の比較"),
                    tr("%1\n\nRMSD（座標そのまま）: %2 Å\n重ね合わせ後のRMSD: %3 Å（水素以外 %4 Å）\n"
                       "重ね合わせ後の最大のずれ: %5 Å")
                        .arg(name)
                        .arg(fit.raw, 0, 'f', 4)
                        .arg(fit.rmsd, 0, 'f', 4)
                        .arg(fit.heavyRmsd, 0, 'f', 4)
                        .arg(fit.maximum, 0, 'f', 4),
                    QMessageBox::Close, parent);
    box.setTextFormat(Qt::PlainText); // The file name is shown as written.
    box.setObjectName("compareResult");
    auto *align = box.addButton(tr("比較対象に重ねる（候補）"), QMessageBox::ActionRole);
    box.exec();
    return box.clickedButton() == align;
}

void editZMatrix(QWidget *parent, EditorContext &editor, UiServices &uiContext) {
    const auto document = [&]() -> const Document & { return editor.document(); };
    QDialog dialog(parent);
    dialog.setObjectName("zMatrixDialog");
    dialog.setWindowTitle(tr("Z-matrix（内部座標）"));
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro =
        new QLabel(tr("原子番号の順に、前の原子を参照して距離・角度・二面角で表します（結合した原子を優先）。"
                      "値を編集すると、その原子の側（結合の先）を動かして「1 分子」に反映します。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *table = new QTableWidget(0, 8);
    table->setObjectName("zMatrixTable");
    table->setHorizontalHeaderLabels({tr("元素"), tr("結合相手"), tr("距離 / Å"), tr("角度相手"),
                                      tr("角度 / °"), tr("二面角相手"), tr("二面角 / °"), ""});
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("テキストをコピー"), QDialogButtonBox::ActionRole);
    copy->setObjectName("zMatrixCopy");
    layout->addWidget(buttons);
    QVector<ZMatrixRow> rows;
    bool filling = false;
    const auto fill = [&] {
        QScopedValueRollback guard(filling, true);
        rows = zMatrix(document());
        table->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            const auto &r = rows[i];
            // Reuse items: fill() also runs from the table's own itemChanged signal.
            const auto cell = [&](int column, const QString &text, bool editable) {
                auto *item = table->item(i, column);
                if (!item) {
                    item = new QTableWidgetItem;
                    table->setItem(i, column, item);
                }
                item->setText(text);
                item->setFlags(editable ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                                        : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            };
            const auto atom = [&](int k) {
                return k >= 0 ? document().atoms[k].element + QString::number(k + 1) : QString();
            };
            cell(0, document().atoms[i].element + QString::number(i + 1), false);
            cell(1, atom(r.bond), false);
            cell(2, r.bond >= 0 ? QLocale().toString(r.distance, 'f', 4) : QString(), r.bond >= 0);
            cell(3, atom(r.angle), false);
            cell(4, r.angle >= 0 ? QLocale().toString(r.angleValue, 'f', 2) : QString(), r.angle >= 0);
            cell(5, atom(r.dihedral), false);
            cell(6, r.dihedral >= 0 ? QLocale().toString(r.dihedralValue, 'f', 2) : QString(),
                 r.dihedral >= 0);
            cell(7, QString(), false);
        }
    };
    fill();
    QObject::connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem *item) {
        if (filling || item->row() >= rows.size())
            return;
        const auto &r = rows[item->row()];
        const auto id = [&](int k) { return document().atoms[k].id; };
        Ids ids;
        if (item->column() == 2)
            ids = {id(r.bond), id(item->row())};
        else if (item->column() == 4)
            ids = {id(r.angle), id(r.bond), id(item->row())};
        else if (item->column() == 6)
            ids = {id(r.dihedral), id(r.angle), id(r.bond), id(item->row())};
        bool ok;
        const double value = QLocale().toDouble(item->text(), &ok);
        if (ids.isEmpty() || !ok) {
            fill();
            return;
        }
        uiContext.safe([&] {
            editor.edit(tr("Z-matrixの値を変更"), [&](Document &d) { setDimension(d, ids, value); });
        });
        fill(); // Other references move with the edited side.
    });
    QObject::connect(copy, &QPushButton::clicked, &dialog, [&] {
        QApplication::clipboard()->setText(zMatrixText(document()));
        uiContext.showMessage(tr("Z-matrixをコピーしました。"), 5000);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

std::optional<SymmetryChoice> askSymmetrization(QWidget *parent, EditorContext &editor,
                                                UiServices &uiContext) {
    QDialog dialog(parent);
    dialog.setObjectName("symmetryDialog");
    dialog.setWindowTitle(tr("点群・対称化"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new FormLayout;
    layout->addLayout(form);
    auto *tolerance = new QDoubleSpinBox;
    tolerance->setObjectName("symmetryTolerance");
    tolerance->setRange(.001, .5);
    tolerance->setDecimals(3);
    tolerance->setSingleStep(.01);
    tolerance->setValue(uiContext.preference(Preference::symmetryTolerance, .05).toDouble());
    tolerance->setSuffix(" Å");
    form->addRow(tr("許容差（原子の位置）"), tolerance);
    auto *result = new QLabel;
    result->setObjectName("symmetryResult");
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(result);
    auto *note =
        new QLabel(tr("全原子（ゴースト・同位体は別の種類）で判定します。対称化は対称操作で座標を平均し、"
                      "候補として表示します。NWChemの入力はnoautosymのため、計算では対称性を使いません。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *apply = buttons->addButton(tr("対称化（候補にする）"), QDialogButtonBox::AcceptRole);
    apply->setObjectName("symmetryApply");
    layout->addWidget(buttons);
    PointGroup group;
    const auto evaluate = [&] {
        group = pointGroup(editor.displayDocument(), tolerance->value());
        uiContext.setPreference(Preference::symmetryTolerance, tolerance->value());
        if (group.name.isEmpty())
            result->setText(editor.displayDocument().atoms.size() > 200
                                ? tr("200原子を超える構造は判定しません。")
                                : tr("点群を決められませんでした。"));
        else
            result->setText(
                tr("点群: %1（対称操作 %2、対称な位置からの最大のずれ %3 Å）")
                    .arg(group.name)
                    .arg(group.name.contains("∞") ? QString("∞") : QString::number(group.operations.size()))
                    .arg(group.deviation, 0, 'f', 4));
        apply->setEnabled(!group.name.isEmpty() && group.name != "C1" && group.name != "Kh" &&
                          !editor.hasCandidate() && !editor.chemistryRunning());
    };
    evaluate();
    QObject::connect(tolerance, &QDoubleSpinBox::valueChanged, &dialog, evaluate);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return SymmetryChoice{tolerance->value(), group.name};
}

std::optional<SolvationChoice> askSolvation(QWidget *parent, UiServices &uiContext, int selected) {
    QDialog dialog(parent);
    dialog.setObjectName("solvationDialog");
    dialog.setWindowTitle(tr("溶媒和殻を配置"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        tr("溶媒分子を、溶質に接する位置から外側へ重ならないように置きます。陽イオン・極性原子には溶媒の"
           "酸素などを、陰イオンには水素を向けます。溶媒分子は形を変えずに置くため、計算の前に"
           "「xTBで構造を最適化」や力場で整えてください。結果は候補として表示します。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new FormLayout;
    layout->addLayout(form);
    auto *solvent = new QComboBox;
    solvent->setObjectName("solvationSolvent");
    for (const auto &id : solventTemplates())
        solvent->addItem(templateName(findTemplate(id)), id);
    solvent->setCurrentIndex(
        std::max(0, solvent->findData(uiContext.preference(Preference::solvationSolvent).toString())));
    form->addRow(tr("溶媒"), solvent);
    auto *count = new QSpinBox;
    count->setObjectName("solvationCount");
    count->setRange(1, 1000);
    count->setValue(uiContext.preference(Preference::solvationCount, 6).toInt());
    count->setToolTip(tr("置ける場所が足りないときは、置けた数だけ置きます。"));
    form->addRow(tr("溶媒分子の数"), count);
    auto *around = new QCheckBox(tr("選択した原子のまわりに置く（%n原子）", nullptr, selected));
    around->setObjectName("solvationAroundSelection");
    around->setEnabled(selected > 0);
    around->setChecked(selected > 0);
    around->setToolTip(tr("外すと分子全体のまわりに置きます。"));
    form->addRow(QString(), around);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("配置（候補にする）"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    uiContext.setPreference(Preference::solvationSolvent, solvent->currentData());
    uiContext.setPreference(Preference::solvationCount, count->value());
    return SolvationChoice{solvent->currentData().toString(), count->value(), around->isChecked()};
}

void preparePdb(QWidget *parent, EditorContext &editor, UiServices &uiContext) {
    const auto document = [&]() -> const Document & { return editor.document(); };
    if (editor.hasCandidate())
        fail(tr("先に候補を確定または取消してください。"));
    if (document().atoms.isEmpty())
        fail(tr("PDBファイルを開いてから診断してください。"));
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("PDBの診断・修復"));
    dialog.resize(820, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(tr("残基を選ぶと診断を表示します。修復は3D画面で候補を確認して確定します。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget;
    layout->addWidget(list);
    for (const auto &r : document().residues) {
        auto *item = new QListWidgetItem(
            QString("%1 %2  ·  %3 / %4").arg(r.code).arg(r.number).arg(r.nTerminal, r.cTerminal), list);
        item->setData(Qt::UserRole, r.id);
    }
    auto *report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setPlainText(structureReport(document()));
    layout->addWidget(report, 1);
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        if (!item)
            return;
        uiContext.safe([&] {
            report->setPlainText(
                residueDiagnostics(document(), item->data(Qt::UserRole).toString()).join('\n'));
        });
    });
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto add = [&](QString text, auto fn) {
        auto *b = new QPushButton(text);
        actions->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, &dialog, [&, fn] { uiContext.safe(fn); });
        return b;
    };
    add(tr("3Dで選択箇所を見る"), [&] {
        if (!list->currentItem())
            fail(tr("残基を選んでください。"));
        editor.setSelection(
            residue(document(), list->currentItem()->data(Qt::UserRole).toString()).atoms.values());
        dialog.accept();
    });
    add(tr("修復候補を作成"), [&] {
        if (!list->currentItem())
            fail(tr("残基を選んでください。"));
        auto rid = list->currentItem()->data(Qt::UserRole).toString();
        editor.edit(tr("PDBの欠損原子を補完"), [=](Document &d) { repairResidue(d, rid); }, true);
        dialog.accept();
    });
    add(tr("残基を再認識"), [&] {
        editor.edit(tr("PDBの残基を認識"), [](Document &d) { recognizeResidues(d); }, true);
        dialog.accept();
    })->setEnabled(hasStereochemistryBackend());
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
} // namespace mf
