// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/macromolecule.h"

namespace mf {
// The contents of a PDB file, and the part of it to read.
MacromoleculeContents pdbContents(const QString &text);
// The records of the selection for Open Babel: the ATOM/HETATM records of one model at one alternate location
// (without the mark), of the chosen chains and kinds, and the CONECT records between them.
QString preparePdb(const QString &text, const MacromoleculeSelection &selection);
} // namespace mf
