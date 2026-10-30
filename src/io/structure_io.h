// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/pdb.h"
#include "model/document.h"

namespace mf {
// A project replaces all fields. Other formats carry a structure; NWChem/Gaussian also carry
// charge and multiplicity, and NWChem calculation settings. PDB and mmCIF files read the selection; a
// multi-frame XYZ requires xyzFrame.
Document importStructure(const QString &path, const MacromoleculeSelection &selection = {},
                         int xyzFrame = -1);
void exportStructure(const Document &document, const QString &path);
QStringList xyzFrames(const QString &text);
Document gaussianStructure(const QString &text);
} // namespace mf
