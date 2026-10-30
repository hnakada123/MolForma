// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/pdb.h"
#include "model/document.h"

namespace mf {
// An atom of the _atom_site table of a PDBx/mmCIF file. Residues and chains take the author's names (auth_*),
// which the PDB file of the same entry uses, and the PDBx names (label_*) where those are missing.
struct MmcifAtom {
    QString element, name, residue, chain, insertion, altloc, entity;
    int number = 0, model = 1, charge = 0;
    double occupancy = 1;
    Vec position = Vec::Zero();
    bool hetatm = false;
    // From the type of the entity (_entity.type), else from the record (group_PDB) and water names.
    ResidueKind kind = ResidueKind::Polymer;
};
struct MmcifEntry {
    QVector<MmcifAtom> atoms;             // The _atom_site table, in file order.
    QHash<QString, QString> descriptions; // The entities' names (_entity.pdbx_description) by ID.
};
// The first data block.
MmcifEntry readMmcif(const QByteArray &text);
MacromoleculeContents mmcifContents(const MmcifEntry &entry);
// The selected atoms, as preparePdb() selects them from a PDB file.
QVector<MmcifAtom> selectMmcif(const QVector<MmcifAtom> &atoms, const MacromoleculeSelection &selection);
} // namespace mf
