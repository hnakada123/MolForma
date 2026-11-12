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
} // namespace mf
