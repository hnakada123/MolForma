// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/cube_combine_dialog.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "ui/editor_context.h"
#include "ui/line_icons.h"
#include <QtWidgets>

namespace mf {
std::optional<CubeCombination> askCubeCombination(QWidget *parent, UiServices &uiContext,
                                                  const QString &source, int dataset) {
    QDialog dialog(parent);
    dialog.setObjectName("resultsCombineDialog");
    dialog.setWindowTitle(tr("CUBEの差・和"));
    dialog.resize(640, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        tr("CUBEに係数を掛けて足し合わせ、新しいCUBEとして保存して表示します。差電子密度 "
           "ρ(AB) − ρ(A) − ρ(B) は、複合体のCUBEに +1、各部分のCUBEに −1 を指定します。"
           "格子点数・原点・間隔が同じCUBEだけを組み合わせられます（「詳細設定」の「軌道ファイル」"
           "でX・Y・Zの範囲と分割数を同じ値にして出力します）。原子は1行目のCUBEのものを書きます。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *table = new QTableWidget(0, 2);
    table->setObjectName("resultsCombineTerms");
    table->setHorizontalHeaderLabels({tr("係数"), tr("CUBEファイル")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table, 1);
    const auto addRow = [table](const QString &path, double factor) {
        const int row = table->rowCount();
        table->insertRow(row);
        auto *spin = new QDoubleSpinBox;
        spin->setObjectName(QString("resultsCombineFactor%1").arg(row));
        spin->setRange(-1000, 1000);
        spin->setDecimals(4);
        spin->setValue(factor);
        table->setCellWidget(row, 0, spin);
        auto *item = new QTableWidgetItem(QDir::toNativeSeparators(path));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 1, item);
    };
    addRow(source, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(lineIcon("plus"), tr("CUBEを追加…"));
    add->setObjectName("resultsCombineAdd");
    auto *remove = new QPushButton(tr("選択行を削除"));
    remove->setObjectName("resultsCombineRemove");
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        const auto files = QFileDialog::getOpenFileNames(
            &dialog, tr("組み合わせるCUBE"), QFileInfo(source).absolutePath(), tr("CUBE (*.cube *.cub)"));
        for (const auto &file : files)
            addRow(file, -1);
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
        const int row = table->currentRow();
        if (row >= 0 && table->rowCount() > 1)
            table->removeRow(row);
    });
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(new QLabel(tr("保存先")));
    const QFileInfo sourceFile(source);
    auto *output = new QLineEdit(sourceFile.dir().filePath(sourceFile.completeBaseName() + "_combined.cube"));
    output->setObjectName("resultsCombineOutput");
    outputRow->addWidget(output, 1);
    auto *browse = new QPushButton(tr("参照…"));
    outputRow->addWidget(browse);
    layout->addLayout(outputRow);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, tr("CUBEの保存先"), output->text(),
                                                       tr("CUBE (*.cube *.cub)"));
        if (!path.isEmpty())
            output->setText(path);
    });
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(tr("作成して表示"));
    box->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    layout->addWidget(box);
    std::optional<CubeCombination> chosen;
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, [&] {
        uiContext.safe([&] {
            if (table->rowCount() < 2)
                fail(tr("組み合わせるCUBEを2つ以上指定してください。"));
            const auto target = output->text().trimmed();
            if (target.isEmpty())
                fail(tr("保存先を指定してください。"));
            QVector<CubeTerm> terms;
            for (int row = 0; row < table->rowCount(); ++row)
                terms.append({table->item(row, 1)->data(Qt::UserRole).toString(),
                              static_cast<QDoubleSpinBox *>(table->cellWidget(row, 0))->value(),
                              row == 0 ? dataset : 0});
            if (QFileInfo::exists(target) &&
                QMessageBox::question(
                    &dialog, tr("CUBEの差・和"),
                    tr("%1 は既にあります。置き換えますか？").arg(QFileInfo(target).fileName()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            chosen = CubeCombination{terms, target};
            dialog.accept();
        });
    });
    dialog.exec();
    return chosen;
}
} // namespace mf
