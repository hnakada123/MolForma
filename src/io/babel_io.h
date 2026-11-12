// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/pdb.h"
#include "model/document.h"

namespace mf {
// Native Open Babel formats and PDBx/mmCIF (.cif, .mmcif), of which PDB and mmCIF read the selection; routing
// of project/XYZ/NWChem/Gaussian files is in io/structure_io.h.
Document importBabelStructure(const QString &path, const MacromoleculeSelection &selection = {});
void exportBabelStructure(const Document &document, const QString &path);
// The SMILES (with stereochemistry) of the first molecule of a MOL/SDF file, such as a 2D structure that
// fromSmiles makes 3D.
QString structureSmiles(const QString &path);
} // namespace mf
