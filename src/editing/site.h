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
struct SiteCutOptions {
    // The kept atoms that receive a hydrogen at a cut are fixed, so that the model keeps the shape it had in
    // the whole.
    bool fixCuts = true;
    bool hydrogens = false; // protonate() the site.
    // The charged groups of the atoms removed within chargeRadius (Å) of the kept atoms stay as point charges
    // (source "site"): the side chains of Asp and Glu (−1), Lys and Arg (+1) and charged His (HIP, +1), the
    // ends of chains, sulfate (−2), the ions of protonate(), and the formal charges of the atoms of other
    // residues. Water and neutral groups leave none.
    bool charges = false;
    double chargeRadius = 15;
};
// Removes every atom but `kept`. Each covalent bond so cut ends in a hydrogen on the kept atom, along the
// bond at the X–H bond length (a link atom); bonds to metals and dative bonds end without one. The total
// charge follows (followingCharge). Returns the hydrogens added at the cuts.
Ids cutOutSite(Document &d, const Ids &kept, const SiteCutOptions &options = {});
// Hydrogens for a structure without them, such as one from the PDB, in the states of pH 7: bonds to metals
// become dative bonds from the donor; amino acids take Asp and Glu −1, Lys and Arg +1, the ends of chains
// charged (NH3+, COO−), Cys bound to a metal −1 and His neutral with its hydrogen on the nitrogen not bound
// to a metal (HIE when neither is), or the state their name tells (HID, HIE, HIP, ASH, GLH, LYN, CYM); ions
// of the PDB's chemical components take their charge (ZN +2, NA +1, CL −1, …) and sulfate (SO4) −2. Then
// every atom gets hydrogens by its valence, as completeHydrogens(): other ligands only so. The total charge
// follows (followingCharge).
void protonate(Document &d);
} // namespace mf
