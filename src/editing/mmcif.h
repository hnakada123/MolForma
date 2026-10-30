// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// An atom of the _atom_site table of a PDBx/mmCIF file. Residues and chains take the author's names
// (auth_*), which the PDB file of the same entry uses, and the PDBx names (label_*) where those are missing.
struct MmcifAtom {
    QString element, name, residue, chain, insertion, altloc;
    int number = 0, model = 1, charge = 0;
    double occupancy = 1;
    Vec position = Vec::Zero();
};
// The _atom_site table of the first data block, in file order.
QVector<MmcifAtom> mmcifAtoms(const QByteArray &text);
// The model numbers (pdbx_PDB_model_num) in file order, as pdbModels() lists the MODEL records.
QStringList mmcifModels(const QVector<MmcifAtom> &atoms);
// One model at one alternate location, chosen as preparePdb() chooses them in a PDB file: an empty altloc
// keeps, in each residue, the location with the highest total occupancy.
QVector<MmcifAtom> selectMmcif(const QVector<MmcifAtom> &atoms, int model, const QString &altloc);
} // namespace mf
