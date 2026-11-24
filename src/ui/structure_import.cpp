// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/structure_import.h"
#include "foundation/i18n.h"
#include "foundation/structure_file.h"
#include "io/crystal.h"
#include "io/mmcif.h"
#include "io/structure_io.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>

namespace mf {
// The wait cursor while a file is read, until the dialogs or the document show.
class Reading {
  public:
    Reading() {
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~Reading() {
        done();
    }
    void done() {
        if (std::exchange(waiting, false))
            QApplication::restoreOverrideCursor();
    }

  private:
    bool waiting = true;
};
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
    // An entry of more atoms than MolForma opens (a virus capsid) starts with the chains from the top that it
    // does open, and shows what is chosen.
    const bool large = contents.atoms() > MacromoleculeAtomLimit;
    QListWidget *chains = nullptr;
    if (contents.chains.size() > 1) {
        chains = new QListWidget;
        chains->setObjectName("importChains");
        int counted = 0;
        bool fits = true;
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
            const int atoms = chain.atoms[0] + chain.atoms[1] + chain.atoms[2];
            fits = fits && (!counted || counted + atoms <= MacromoleculeAtomLimit);
            if (fits)
                counted += atoms;
            item->setCheckState(!large || fits ? Qt::Checked : Qt::Unchecked);
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
    QLabel *size = nullptr;
    if (large) {
        size = new QLabel;
        size->setObjectName("importAtoms");
        size->setWordWrap(true);
        form->addRow(size);
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
    // The chains and kinds chosen; a kind the entry does not have stays chosen, as in the files without a
    // choice.
    const auto chosen = [&] {
        auto next = selection;
        if (chains && chosenChains().size() < chains->count())
            next.chains = chosenChains();
        next.polymer = !kindBoxes[0] || kindBoxes[0]->isChecked();
        next.ligands = !kindBoxes[1] || kindBoxes[1]->isChecked();
        next.water = !kindBoxes[2] || kindBoxes[2]->isChecked();
        return next;
    };
    // Something must be left to read, and no more than MolForma opens.
    const auto update = [&] {
        const int atoms = large ? contents.atoms(chosen()) : 0;
        if (size)
            size->setText(tr("選んだ範囲: %1原子（MolFormaで開けるのは%2原子まで）")
                              .arg(atoms)
                              .arg(MacromoleculeAtomLimit));
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled((!chains || !chosenChains().isEmpty()) &&
                         (kinds < 2 || std::any_of(kindBoxes.begin(), kindBoxes.end(), checked)) &&
                         atoms <= MacromoleculeAtomLimit);
    };
    update();
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
    const auto next = chosen();
    selection.chains = next.chains;
    selection.polymer = next.polymer;
    selection.ligands = next.ligands;
    selection.water = next.water;
    return selection;
}
// What to read of a small-molecule crystal structure: the molecules that complete the asymmetric unit (one
// copy each, with counterions and solvent) or all of the unit cell, and the minor disorder groups. The dialog
// appears only where the file offers a choice.
static std::optional<CrystalSelection> chooseCrystal(QWidget *parent, const Crystal &crystal, bool ask) {
    CrystalSelection selection;
    const auto contents = crystalContents(crystal);
    if (!ask || (contents.cellMolecules <= contents.molecules && !contents.disorder))
        return selection;
    QDialog dialog(parent);
    dialog.setObjectName("crystalImportDialog");
    dialog.setWindowTitle(tr("読み込む範囲（結晶構造）"));
    auto *form = new QFormLayout(&dialog);
    auto *about = new QLabel(tr("%1 · 空間群 %2 · 非対称単位 %n 原子", nullptr, contents.asymmetricAtoms)
                                 .arg(crystal.formula.isEmpty() ? crystal.name : crystal.formula,
                                      crystal.spaceGroup.isEmpty() ? QString("?") : crystal.spaceGroup));
    about->setWordWrap(true);
    form->addRow(about);
    auto *molecules = new QRadioButton(
        tr("化学式単位（%n個の分子。非対称単位の分子を完成させる）", nullptr, contents.molecules));
    molecules->setObjectName("crystalMolecules");
    molecules->setChecked(true);
    auto *cell = new QRadioButton(
        tr("単位格子の内容（%n個の分子、%1原子）", nullptr, contents.cellMolecules).arg(contents.cellAtoms));
    cell->setObjectName("crystalCell");
    form->addRow(tr("範囲"), molecules);
    form->addRow(QString(), cell);
    QCheckBox *minor = nullptr;
    if (contents.disorder) {
        minor = new QCheckBox(tr("乱れの副成分（disorder group 2以降）も読み込む"));
        minor->setObjectName("crystalMinor");
        form->addRow(tr("乱れ"), minor);
    }
    if (contents.polymeric) {
        auto *note = new QLabel(
            tr("結合が格子の外へ続く構造（高分子・ネットワーク）です。単位格子の中で切って読み込みます。"));
        note->setWordWrap(true);
        form->addRow(note);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    selection.cell = cell->isChecked();
    selection.minor = minor && minor->isChecked();
    return selection;
}
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path, bool ask) {
    StructureImportOptions options;
    const auto suffix = structureSuffix(path);
    bool ok = true;
    const bool pdb = suffix == "pdb" || suffix == "ent", cif = suffix == "cif" || suffix == "mmcif";
    if (pdb || cif) {
        Reading reading;
        const auto bytes = readStructureFile(path);
        if (cif && crystalCif(bytes)) {
            const auto read = readCrystal(bytes);
            reading.done();
            const auto crystal = chooseCrystal(parent, read, ask);
            if (!crystal)
                return std::nullopt;
            options.crystal = *crystal;
            return options;
        }
        // The entry read here is what the structure is read from, not the file again.
        if (cif)
            options.mmcif = std::make_shared<const MmcifEntry>(readMmcif(bytes));
        const auto contents = pdb ? pdbContents(QString::fromUtf8(bytes)) : mmcifContents(*options.mmcif);
        reading.done();
        const auto selection = chooseMacromolecule(parent, contents, ask);
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
StructureImport importChosenStructure(const QString &path, const StructureImportOptions &options) {
    Reading reading;
    return importStructureWithNotes(path, options);
}
} // namespace mf
