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
#include <openbabel/obfunctions.h>
#include <openbabel/residue.h>
#include <openbabel/stereo/stereo.h>

namespace mf {
using namespace OpenBabel;
// The atoms of a PDBx/mmCIF file, made a molecule the way Open Babel reads a PDB file: atoms with their
// residues, bonds by distance, bond orders from the geometry and typical hydrogen counts, which aromaticity
// depends on. Its residue templates add no bonds there (they look up atom names without the PDB columns'
// padding), so they are left out here too.
static Document mmcifStructure(const QVector<MmcifAtom> &atoms) {
    OBMol mol;
    mol.SetChainsPerceived(); // The residues are the file's.
    mol.BeginModify();
    QHash<QString, OBResidue *> residues;
    QHash<OBResidue *, QString> chains;
    for (const auto &a : atoms) {
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
        o->SetAtomicNum(OBElements::GetAtomicNum(a.element.toLatin1().constData()));
        o->SetVector(a.position.x(), a.position.y(), a.position.z());
        o->SetFormalCharge(a.charge);
        residue->AddAtom(o);
        residue->SetAtomID(o, a.name.toStdString());
    }
    mol.EndModify();
    mol.ConnectTheDots();
    mol.PerceiveBondOrders();
    mol.SetChainsPerceived(); // EndModify() cleared it.
    for (unsigned int i = 1; i <= mol.NumAtoms(); ++i)
        OBAtomAssignTypicalImplicitHydrogens(mol.GetAtom(i));
    return fromOB(mol, true, chains);
}
Document importBabelStructure(const QString &path, const MacromoleculeSelection &selection) {
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
    Document d;
    if (ext == "mmcif") {
        d = mmcifStructure(selectMmcif(readMmcif(bytes).atoms, selection));
    } else {
        if (ext == "pdb")
            bytes = preparePdb(QString::fromUtf8(bytes), selection).toUtf8();
        OBConversion conversion;
        if (!conversion.SetInFormat(ext.toLatin1().constData()))
            fail(tr("対応していない形式です: %1").arg(ext));
        OBMol mol;
        if (!conversion.ReadString(&mol, bytes.toStdString()) || !mol.NumAtoms())
            fail(tr("構造ファイルを読み込めません。"));
        d = fromOB(mol, ext == "pdb");
    }
    auto meta = d.data["metadata"].toObject();
    meta["source_file"] = path;
    meta["source_format"] = ext;
    if (ext == "pdb" || ext == "mmcif") {
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
    }
    d.data["metadata"] = meta;
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
