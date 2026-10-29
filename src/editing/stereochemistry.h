// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
namespace mf {
bool hasStereochemistryBackend();
void recordStereo(Document &d);
// Residues from the bond graph; the charges it reads from the hydrogens carry the total charge along
// (followingCharge).
void recognizeResidues(Document &d);
} // namespace mf
