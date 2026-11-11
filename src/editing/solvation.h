// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// The solvents a shell can be made of, as their template IDs ("skeleton:water", …).
QStringList solventTemplates();
struct SolvationOptions {
    int count = 6;
    // The atoms the solvent gathers around; empty: the whole molecule.
    Ids around;
};
// Places count solvent molecules (rigid copies of the template) around the atoms: first in contact with
// them, at a distance that suits each (an ion, a polar atom, carbon), then outward around the solvent
// already placed, as evenly spread as the room allows and never closer than contact to anything. The donor
// atom of the solvent (O of water) faces a cation or a polar atom; its hydrogens face an anion. Returns the
// number placed, which may be fewer than count; fails when none fits.
int solvate(Document &document, const QJsonObject &solvent, const SolvationOptions &options);
} // namespace mf
