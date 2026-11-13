// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/macromolecule.h"

namespace mf {
bool MacromoleculeSelection::keeps(const QString &chain, ResidueKind kind) const {
    return (chains.isEmpty() || chains.contains(chain)) && (kind == ResidueKind::Polymer  ? polymer
                                                            : kind == ResidueKind::Ligand ? ligands
                                                                                          : water);
}
void MacromoleculeCount::add(const QString &residue, const QString &chain, const QString &code,
                             ResidueKind kind, const QString &description) {
    if (residues.contains(residue))
        return;
    residues.insert(residue);
    auto it = chains.find(chain);
    if (it == chains.end()) {
        it = chains.insert(chain, contents.chains.size());
        contents.chains.append({chain, {}, 0, {}});
    }
    auto &entry = contents.chains[*it];
    if (entry.description.isEmpty())
        entry.description = description;
    if (kind != ResidueKind::Polymer && !entry.groups.contains(code))
        entry.groups << code;
    switch (kind) {
    case ResidueKind::Polymer:
        ++contents.polymers;
        ++entry.residues;
        break;
    case ResidueKind::Ligand:
        ++contents.ligands;
        break;
    case ResidueKind::Water:
        ++contents.waters;
    }
}
void AltlocChoice::add(const QString &residue, const QString &altloc, double occupancy) {
    if (!altloc.isEmpty())
        occupancies[residue][altloc] += occupancy;
}
bool AltlocChoice::keeps(const QString &residue, const QString &altloc) const {
    if (altloc.isEmpty())
        return true;
    if (!chosen.isEmpty())
        return altloc == chosen;
    QString preferred;
    double best = -1;
    const auto locations = occupancies.value(residue);
    for (auto it = locations.begin(); it != locations.end(); ++it)
        if (it.value() > best) {
            best = it.value();
            preferred = it.key();
        }
    return altloc == preferred;
}
} // namespace mf
