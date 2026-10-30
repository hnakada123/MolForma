// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/structure_import.h"
#include "editing/mmcif.h"
#include "io/structure_io.h"
#include "model/i18n.h"
#include "model/structure_file.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QListWidget>
#include <QPushButton>

namespace mf {
// What to read of a PDB or mmCIF entry, chosen in one dialog: the model, the alternate location, the chains
// and the kinds of residues. The dialog appears only where the entry offers a choice.
static std::optional<MacromoleculeSelection>
chooseMacromolecule(QWidget *parent, const MacromoleculeContents &contents, bool ask) {
    MacromoleculeSelection selection;
    selection.model = contents.models.first().toInt();
    const int kinds = (contents.polymers > 0) + (contents.ligands > 0) + (contents.waters > 0);
    if (!ask || (contents.models.size() < 2 && !contents.altlocs && contents.chains.size() < 2 && kinds < 2))
        return selection;
    QDialog dialog(parent);
    dialog.setObjectName("structureImportDialog");
    dialog.setWindowTitle(tr("読み込む範囲"));
    auto *form = new QFormLayout(&dialog);
    QComboBox *model = nullptr, *altloc = nullptr;
    if (contents.models.size() > 1) {
        model = new QComboBox;
        model->setObjectName("importModel");
        model->addItems(contents.models);
        form->addRow(tr("読み込むモデル"), model);
    }
    const auto automatic = tr("自動");
    if (contents.altlocs) {
        altloc = new QComboBox;
        altloc->setObjectName("importAltloc");
        altloc->setEditable(true);
        altloc->addItems({automatic, "A", "B", "C"});
        form->addRow(tr("代替位置（自動: 残基ごとの占有率最大）"), altloc);
    }
    QListWidget *chains = nullptr;
    if (contents.chains.size() > 1) {
        chains = new QListWidget;
        chains->setObjectName("importChains");
        for (const auto &chain : contents.chains) {
            auto label = chain.id.isEmpty() ? QString("-") : chain.id;
            // A chain of ligands or water alone shows their names.
            const auto name =
                chain.description.isEmpty() && !chain.residues ? chain.groups.join(", ") : chain.description;
            if (!name.isEmpty())
                label += "  " + name;
            if (chain.residues)
                label += tr("（%n残基）", nullptr, chain.residues);
            auto *item = new QListWidgetItem(label, chains);
            item->setData(Qt::UserRole, chain.id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        chains->setMinimumWidth(std::min(chains->sizeHintForColumn(0) + 40, 720));
        form->addRow(tr("鎖"), chains);
    }
    QList<QCheckBox *> kindBoxes; // Polymer, ligands and water, where the entry has more than one kind.
    for (const auto &[count, text, name] :
         {std::tuple{contents.polymers, tr("たんぱく質・核酸（%n残基）", nullptr, contents.polymers),
                     "importPolymer"},
          {contents.ligands, tr("配位子・イオン・糖鎖など（%n個）", nullptr, contents.ligands),
           "importLigands"},
          {contents.waters, tr("水（%n分子）", nullptr, contents.waters), "importWater"}}) {
        QCheckBox *box = nullptr;
        if (kinds > 1 && count) {
            box = new QCheckBox(text);
            box->setObjectName(name);
            box->setChecked(true);
            form->addRow(kindBoxes.size() == 0 ? tr("成分") : QString(), box);
        }
        kindBoxes << box;
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto checked = [&](QCheckBox *box) { return box && box->isChecked(); };
    const auto chosenChains = [&] {
        QStringList ids;
        for (int i = 0; chains && i < chains->count(); ++i)
            if (chains->item(i)->checkState() == Qt::Checked)
                ids << chains->item(i)->data(Qt::UserRole).toString();
        return ids;
    };
    // Something must be left to read.
    const auto update = [&] {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled((!chains || !chosenChains().isEmpty()) &&
                         (kinds < 2 || std::any_of(kindBoxes.begin(), kindBoxes.end(), checked)));
    };
    if (chains)
        QObject::connect(chains, &QListWidget::itemChanged, &dialog, update);
    for (auto *box : kindBoxes)
        if (box)
            QObject::connect(box, &QCheckBox::toggled, &dialog, update);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    if (model)
        selection.model = model->currentText().toInt();
    if (altloc && altloc->currentText() != automatic)
        selection.altloc = altloc->currentText().trimmed();
    if (chains && chosenChains().size() < chains->count())
        selection.chains = chosenChains();
    // A kind the entry does not have stays chosen, as in the files without a choice.
    selection.polymer = !kindBoxes[0] || kindBoxes[0]->isChecked();
    selection.ligands = !kindBoxes[1] || kindBoxes[1]->isChecked();
    selection.water = !kindBoxes[2] || kindBoxes[2]->isChecked();
    return selection;
}
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path, bool ask) {
    StructureImportOptions options;
    const auto suffix = structureSuffix(path);
    bool ok = true;
    const bool pdb = suffix == "pdb" || suffix == "ent";
    if (pdb || suffix == "cif" || suffix == "mmcif") {
        const auto bytes = readStructureFile(path);
        const auto selection = chooseMacromolecule(
            parent, pdb ? pdbContents(QString::fromUtf8(bytes)) : mmcifContents(readMmcif(bytes)), ask);
        if (!selection)
            return std::nullopt;
        options.selection = *selection;
    } else if (suffix == "xyz") {
        const auto frames = xyzFrames(QString::fromUtf8(readStructureFile(path)));
        options.frame = 0;
        if (!ask) {
            options.frame = int(frames.size()) - 1;
        } else if (frames.size() > 2000) {
            options.frame =
                QInputDialog::getInt(
                    parent, tr("XYZのフレーム"),
                    tr("%1フレームのXYZです。読み込むフレーム番号（既定: 最後）").arg(frames.size()),
                    frames.size(), 1, frames.size(), 1, &ok) -
                1;
        } else if (frames.size() > 1) {
            QStringList labels;
            for (int i = 0; i < frames.size(); ++i)
                labels << QString("%1: %2").arg(i + 1).arg(frames[i].section('\n', 1, 1).trimmed().left(60));
            const auto choice = QInputDialog::getItem(
                parent, tr("XYZのフレーム"),
                tr("%1フレームのXYZです。読み込む構造（既定: 最後）").arg(frames.size()), labels,
                labels.size() - 1, false, &ok);
            options.frame = labels.indexOf(choice);
        }
        if (!ok)
            return std::nullopt;
    }
    return options;
}
} // namespace mf
