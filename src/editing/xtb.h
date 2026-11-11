// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <limits>

namespace mf {
// Geometry optimization with GFN-xTB: the xtb program of the Grimme group, run as an external program
// that the user installs (as NWChem), never linked or shipped with MolForma. It covers the elements up to
// Rn, metals and coordination compounds included, which the force fields of Open Babel refuse.
struct XtbOptions {
    QString method = "gfn2";  // "gfn2" (GFN2-xTB), "gfn1" (GFN1-xTB) or "gfnff" (GFN-FF).
    QString level = "normal"; // Convergence of xtb --opt: "crude", "loose", "normal" or "tight".
    QString solvent;          // An ALPB solvent of xtbSolvents(), or empty for the gas phase.
    Ids moving;               // Only these atoms move, the others are fixed; empty: all but fixed atoms.
    bool operator==(const XtbOptions &) const = default;
};
QStringList xtbMethods();
QString xtbMethodName(const QString &method); // "GFN2-xTB"
QStringList xtbLevels();
QString xtbLevelName(const QString &level); // In words.
QStringList xtbSolvents();                  // ALPB solvents, in xtb's names.
// The xtb executable as named in Preferences (a name looked up in PATH, or a path), or empty when it
// is not found or cannot be run.
QString xtbExecutable(const QString &setting);
// Writes the input of an optimization of the document into a folder (input.xyz, and xcontrol for the
// fixed atoms) and returns xtb's arguments. The charge and multiplicity are the document's. Fails for
// documents xtb cannot take (ghosts, elements beyond Rn, no atom to move).
QStringList prepareXtb(const Document &document, const XtbOptions &options, const QString &folder);
struct XtbResult {
    Document document; // The document with its atoms where xtb left them; fixed atoms exactly in place.
    double energy = std::numeric_limits<double>::quiet_NaN(); // Hartree, of the last structure.
    bool converged = false;
    double rmsd = 0, largest = 0; // How far the atoms moved (Å).
};
// Reads what xtb left in the folder (xtbopt.xyz, and its output for the verdict): the structure of
// the same atoms in the same order. A molecule without fixed atoms is fitted back onto the original,
// so that it does not turn or move as a whole. Fails when xtb gave no structure, with its error.
XtbResult readXtb(const Document &document, const XtbOptions &options, const QString &folder,
                  const QString &output);
} // namespace mf
