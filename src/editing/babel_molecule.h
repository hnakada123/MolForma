// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QHash>
#include <openbabel/mol.h>
#include <openbabel/residue.h>

namespace mf {
// The document as an Open Babel molecule and back, for the code that calls Open Babel's API itself
// (editing/chemistry.cpp, which defines them, and the readers and writers of io/babel_io.cpp).
OpenBabel::OBMol toOB(const Document &d);
// chainNames gives the residues' chains where Open Babel's one character would not hold them.
Document fromOB(OpenBabel::OBMol &mol, bool readResidues = false,
                const QHash<OpenBabel::OBResidue *, QString> &chainNames = {});
// What Open Babel's PDB reader does after its bonds by distance: bond orders from the geometry
// (OBMol::PerceiveBondOrders), typical implicit hydrogen counts, and the aromaticity that depends on both. It
// is done for a few thousand atoms at a time, the molecules (atoms joined by bonds) kept whole: the ring
// search of one OBMol takes time with the cube of its atoms, minutes for a large protein complex read at once
// and days for a virus capsid, while each of their chains is a molecule of a few thousand atoms.
void perceiveBondOrdersByMolecule(OpenBabel::OBMol &mol);
} // namespace mf
