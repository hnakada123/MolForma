// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/structure_import.h"
#include "editing/chemistry.h"
#include "editing/mmcif.h"
#include "io/structure_io.h"
#include "model/i18n.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QRegularExpression>

namespace mf {
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path) {
    StructureImportOptions options;
    const auto suffix = QFileInfo(path).suffix().toLower();
    bool ok = true;
    const bool pdb = suffix == "pdb" || suffix == "ent";
    if (pdb || suffix == "cif" || suffix == "mmcif") {
        QStringList models;
        bool altlocs;
        if (pdb) {
            const auto text = QString::fromUtf8(readFile(path));
            models = pdbModels(text);
            altlocs = text.contains(QRegularExpression("(?m)^(ATOM  |HETATM).{10}[A-Z]"));
        } else {
            const auto atoms = mmcifAtoms(readFile(path));
            models = mmcifModels(atoms);
            altlocs = std::any_of(atoms.begin(), atoms.end(),
                                  [](const MmcifAtom &a) { return !a.altloc.isEmpty(); });
        }
        options.model = models.first().toInt();
        if (models.size() > 1) {
            options.model =
                QInputDialog::getItem(parent, "PDB MODEL", tr("読み込むモデル"), models, 0, false, &ok)
                    .toInt();
            if (!ok)
                return std::nullopt;
        }
        if (altlocs) {
            const auto automatic = tr("自動");
            options.altloc =
                QInputDialog::getItem(parent, "PDB ALTLOC", tr("代替位置（自動: 残基ごとの占有率最大）"),
                                      {automatic, "A", "B", "C"}, 0, true, &ok);
            if (!ok)
                return std::nullopt;
            if (options.altloc == automatic)
                options.altloc.clear();
        }
    } else if (suffix == "xyz") {
        const auto frames = xyzFrames(QString::fromUtf8(readFile(path)));
        options.frame = 0;
        if (frames.size() > 2000) {
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
