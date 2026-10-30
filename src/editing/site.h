// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// A site of a large structure, such as the active site of a protein, cut out as a cluster model for quantum
// chemistry: the atoms around a center (a ligand, residues), the bonds so cut ending in hydrogens.
struct SiteOptions {
    double radius = 5; // Å from any atom of the center.
    // A residue with an atom within the radius comes whole, and so does the molecule of an atom outside
    // residues; otherwise only the atoms within it.
    bool wholeResidues = true;
    bool waters = true; // Water molecules within the radius.
};
// The center and the atoms around it, in the order of the document. Hydrogens go with the atom they are
// bonded to.
Ids siteAtoms(const Document &d, const Ids &center, const SiteOptions &options = {});
// Removes every atom but `kept`. Each covalent bond so cut ends in a hydrogen on the kept atom, along the
// bond at the X–H bond length (a link atom); bonds to metals and dative bonds end without one. fixCuts fixes
// the kept atoms that receive such a hydrogen, so that the model keeps the shape it had in the whole. The
// total charge follows (followingCharge). Returns the hydrogens added.
Ids cutOutSite(Document &d, const Ids &kept, bool fixCuts = true);
} // namespace mf
