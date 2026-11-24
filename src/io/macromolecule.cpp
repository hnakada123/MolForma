// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/macromolecule.h"
#include "foundation/error.h"
#include "foundation/i18n.h"

namespace mf {
void checkMacromoleculeAtoms(qsizetype atoms) {
    if (atoms > MacromoleculeAtomLimit)
        fail(tr("選んだ範囲には%1個の原子があります。MolFormaで開けるのは%2原子までです。読み込む鎖や成分を"
                "減らしてください。")
                 .arg(atoms)
                 .arg(MacromoleculeAtomLimit));
}
bool MacromoleculeSelection::keeps(const QString &chain, ResidueKind kind) const {
    return (chains.isEmpty() || chains.contains(chain)) && (kind == ResidueKind::Polymer  ? polymer
                                                            : kind == ResidueKind::Ligand ? ligands
                                                                                          : water);
}
int MacromoleculeContents::atoms(const MacromoleculeSelection &selection) const {
    int count = 0;
    for (const auto &chain : chains)
        for (const auto kind : {ResidueKind::Polymer, ResidueKind::Ligand, ResidueKind::Water})
            if (selection.keeps(chain.id, kind))
                count += chain.atoms[int(kind)];
    return count;
}
void MacromoleculeCount::add(const QString &residue, const QString &chain, const QString &code,
                             ResidueKind kind, const QString &altloc, const QString &description) {
    // The atoms of one location, as if the first of the residue were read.
    const auto count = [&] {
        if (!altloc.isEmpty()) {
            auto &first = locations[residue];
            if (first.isEmpty())
                first = altloc;
            if (first != altloc)
                return;
        }
        ++contents.chains[current].atoms[int(kind)];
    };
    // The atoms of a residue follow one another, and the residue (whose text names its chain) of the atom
    // before needs no lookup.
    if (current >= 0 && residue == last) {
        count();
        return;
    }
    last = residue;
    auto it = chains.find(chain);
    if (it == chains.end()) {
        it = chains.insert(chain, contents.chains.size());
        contents.chains.append({chain, {}, 0, {}, {}});
    }
    current = *it;
    count();
    if (residues.contains(residue))
        return;
    residues.insert(residue);
    auto &entry = contents.chains[current];
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
