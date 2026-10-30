// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/pdb.h"
#include "model/document.h"
namespace mf {
// Native Open Babel formats and PDBx/mmCIF (.cif, .mmcif), of which PDB and mmCIF read the selection; routing
// of project/XYZ/NWChem/Gaussian files is in io/structure_io.h.
Document importBabelStructure(const QString &path, const MacromoleculeSelection &selection = {});
void exportBabelStructure(const Document &document, const QString &path);
// Reads XYZ text and assigns bond orders from the geometry; XYZ itself carries none.
Document xyzStructure(const QString &text);
Document fromSmiles(const QString &smiles);
Document refine(const Document &d, bool regenerate = false, const Ids &moving = {});
struct Conformer {
    Document document;
    double energy = 0; // Force-field energy in kcal/mol.
};
// Rotor search with the MMFF94 (else UFF) force field: up to `count` distinct conformers, lowest
// energy first, each minimized. The atoms and their order stay; fixed atoms are not allowed.
QVector<Conformer> searchConformers(const Document &d, int count = 10);
} // namespace mf
