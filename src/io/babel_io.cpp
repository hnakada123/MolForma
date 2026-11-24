// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/babel_io.h"
#include "editing/babel_molecule.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "foundation/structure_file.h"
#include "io/mmcif.h"
#include <QFileInfo>
#include <openbabel/atom.h>
#include <openbabel/elements.h>
#include <openbabel/mol.h>
#include <openbabel/obconversion.h>
#include <openbabel/residue.h>
#include <openbabel/stereo/stereo.h>

namespace mf {
using namespace OpenBabel;
// An atom left out for its unknown element, by its residue, chain, number and name.
static QString unknownAtom(const QString &residue, const QString &chain, int number, const QString &insertion,
                           const QString &name) {
    return tr("%1 %2 %3%4 の原子 %5").arg(residue, chain).arg(number).arg(insertion, name);
}
// The atoms of a PDBx/mmCIF file, made a molecule the way Open Babel reads a PDB file: atoms with their
// residues, bonds by distance, bond orders from the geometry and typical hydrogen counts, which aromaticity
// depends on (perceiveBondOrdersByMolecule, as for PDB files). Its residue templates add no bonds there (they
// look up atom names without the PDB columns' padding), so they are left out here too.
static Document mmcifStructure(const QVector<MmcifAtom> &atoms, QStringList &notes) {
    OBMol mol;
    mol.SetChainsPerceived(); // The residues are the file's.
    mol.BeginModify();
    QHash<QString, OBResidue *> residues;
    QHash<OBResidue *, QString> chains;
    for (const auto &a : atoms) {
        const int element = OBElements::GetAtomicNum(a.element.toLatin1().constData());
        if (!element) {
            notes << unknownAtom(a.residue, a.chain, a.number, a.insertion, a.name);
            continue;
        }
        auto *&residue =
            residues[QString("%1\n%2\n%3\n%4").arg(a.chain).arg(a.number).arg(a.insertion, a.residue)];
        if (!residue) {
            residue = mol.NewResidue();
            residue->SetName(a.residue.toStdString());
            residue->SetNum(a.number);
            residue->SetChain(a.chain.isEmpty() ? ' ' : a.chain[0].toLatin1());
            residue->SetInsertionCode(a.insertion.isEmpty() ? 0 : a.insertion[0].toLatin1());
            chains[residue] = a.chain;
        }
        auto *o = mol.NewAtom();
        o->SetAtomicNum(element);
        o->SetVector(a.position.x(), a.position.y(), a.position.z());
        o->SetFormalCharge(a.charge);
        residue->AddAtom(o);
        residue->SetAtomID(o, a.name.toStdString());
    }
    mol.EndModify();
    if (!mol.NumAtoms())
        fail(tr("元素の分かる原子がありません。"));
    mol.ConnectTheDots();
    perceiveBondOrdersByMolecule(mol);
    mol.SetChainsPerceived(); // EndModify() cleared it.
    return fromOB(mol, true, chains);
}
// Where a PDB or mmCIF structure comes from, and the part of the entry read.
static void macromoleculeSource(Document &d, const QString &path, const QString &format,
                                const MacromoleculeSelection &selection) {
    auto meta = d.data["metadata"].toObject();
    meta["source_file"] = path;
    meta["source_format"] = format;
    meta["pdb_model"] = selection.model;
    meta["pdb_altloc"] = selection.altloc;
    meta["pdb_chains"] = QJsonArray::fromStringList(selection.chains); // Empty: every chain.
    QJsonArray kinds;
    for (const auto &[kept, kind] : {std::pair{selection.polymer, "polymer"},
                                     {selection.ligands, "ligands"},
                                     {selection.water, "water"}})
        if (kept)
            kinds << kind;
    meta["pdb_components"] = kinds;
    d.data["metadata"] = meta;
}
Document importMmcif(const MmcifEntry &entry, const QString &path, const MacromoleculeSelection &selection,
                     QStringList *notes) {
    QStringList left;
    auto d = mmcifStructure(selectMmcif(entry.atoms, selection), left);
    if (notes)
        *notes << left;
    macromoleculeSource(d, path, "mmcif", selection);
    return d;
}
Document importBabelStructure(const QString &path, const MacromoleculeSelection &selection,
                              QStringList *notes) {
    QString ext = structureSuffix(path);
    if (ext == "ent")
        ext = "pdb";
    if (ext == "cif")
        ext = "mmcif";
    if (ext == "sd")
        ext = "sdf";
    if (ext == "mdl")
        ext = "mol";
    // Open Babel knows some 150 formats; files from others go only through these common readers.
    if (!QStringList{"mol", "sdf", "mol2", "cml", "pdb", "mmcif"}.contains(ext))
        fail(tr("対応していない形式です: %1").arg(ext));
    auto bytes = readStructureFile(path);
    if (ext == "mmcif")
        return importMmcif(readMmcif(bytes), path, selection, notes);
    if (ext == "pdb")
        bytes = preparePdb(QString::fromUtf8(bytes), selection).toUtf8();
    OBConversion conversion;
    if (!conversion.SetInFormat(ext.toLatin1().constData()))
        fail(tr("対応していない形式です: %1").arg(ext));
    // The PDB reader's bonds by distance, without its bond orders of the whole file at once.
    if (ext == "pdb")
        conversion.AddOption("s", OBConversion::INOPTIONS);
    OBMol mol;
    if (!conversion.ReadString(&mol, bytes.toStdString()) || !mol.NumAtoms())
        fail(tr("構造ファイルを読み込めません。"));
    if (ext == "pdb") {
        // Atoms of unknown elements (element 0 for Open Babel) are left out, out of their residues too.
        QVector<OBAtom *> unknown;
        for (unsigned int i = 1; i <= mol.NumAtoms(); ++i)
            if (!mol.GetAtom(i)->GetAtomicNum())
                unknown << mol.GetAtom(i);
        for (auto *atom : unknown) {
            if (auto *r = atom->GetResidue()) {
                if (notes)
                    *notes << unknownAtom(
                        QString::fromStdString(r->GetName()).trimmed(),
                        QString(QChar(r->GetChain())).trimmed(), r->GetNum(),
                        r->GetInsertionCode() ? QString(QChar(r->GetInsertionCode())).trimmed() : QString(),
                        QString::fromStdString(r->GetAtomID(atom)).trimmed());
                r->RemoveAtom(atom);
            }
            mol.DeleteAtom(atom);
        }
        if (!mol.NumAtoms())
            fail(tr("元素の分かる原子がありません。"));
        mol.SetChainsPerceived(); // Deleting cleared it; the residues are the file's.
        perceiveBondOrdersByMolecule(mol);
    }
    auto d = fromOB(mol, ext == "pdb");
    if (ext == "pdb") {
        macromoleculeSource(d, path, ext, selection);
    } else {
        auto meta = d.data["metadata"].toObject();
        meta["source_file"] = path;
        meta["source_format"] = ext;
        d.data["metadata"] = meta;
    }
    return d;
}
void exportBabelStructure(const Document &d, const QString &path) {
    const auto ext = QFileInfo(path).suffix().toLower();
    auto mol = toOB(d);
    OpenBabel::StereoFrom3D(&mol);
    OBConversion conversion;
    if (!conversion.SetOutFormat(ext.toLatin1().constData()))
        fail(tr("対応していない出力形式です。"));
    auto result = conversion.WriteString(&mol);
    if (result.empty())
        fail(tr("構造を書き出せません。"));
    writeFile(path, QByteArray::fromStdString(result));
}
QString structureSmiles(const QString &path) {
    OBConversion conversion;
    if (!conversion.SetInAndOutFormats("sdf", "smi"))
        fail(tr("対応していない形式です: %1").arg("sdf"));
    OBMol mol;
    if (!conversion.ReadString(&mol, readStructureFile(path).toStdString()) || !mol.NumAtoms())
        fail(tr("構造ファイルを読み込めません。"));
    const auto smiles = QString::fromStdString(conversion.WriteString(&mol)).section('\t', 0, 0).trimmed();
    if (smiles.isEmpty())
        fail(tr("構造ファイルを読み込めません。"));
    return smiles;
}
} // namespace mf
