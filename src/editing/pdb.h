// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QStringList>
#include <QVector>

namespace mf {
// What a PDB or PDBx/mmCIF entry holds, and the part of it to read.

// The residues of an entry: polymer chains (proteins and nucleic acids, with their modified residues), the
// other hetero groups (ligands, ions, sugars) and water.
enum class ResidueKind { Polymer, Ligand, Water };
bool waterResidue(const QString &code);
struct MacromoleculeChain {
    QString id, description; // The molecule's name, where the file gives it (COMPND, _entity).
    int residues = 0;        // Of the polymer.
    QStringList groups;      // The names of the other residues (ligands, water), once each.
};
struct MacromoleculeContents {
    QStringList models;
    bool altlocs = false;
    QVector<MacromoleculeChain> chains;
    int polymers = 0, ligands = 0, waters = 0; // Residues of each kind, in the first model.
};
struct MacromoleculeSelection {
    int model = 1;
    QString altloc;     // Empty: in each residue, the location with the highest total occupancy.
    QStringList chains; // Empty: every chain.
    bool polymer = true, ligands = true, water = true;
    bool keeps(const QString &chain, ResidueKind kind) const;
};
MacromoleculeContents pdbContents(const QString &text);
// The records of the selection for Open Babel: the ATOM/HETATM records of one model at one alternate location
// (without the mark), of the chosen chains and kinds, and the CONECT records between them.
QString preparePdb(const QString &text, const MacromoleculeSelection &selection);
} // namespace mf
